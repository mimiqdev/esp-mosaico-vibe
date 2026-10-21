#!/usr/bin/env python3
"""Read-only HTTP fallback for the Magpie usage display.

Serves the same derived payloads as the BLE server, so the device can fall back
to Wi-Fi when BLE is unavailable:

    GET /v1/display/index        -> INDEX payload
    GET /v1/display/sub/<i>      -> SUB payload for slot <i>
    GET /v1/display/health       -> {"ok":true,"t":...,"c":...,"age_s":...}

The server only proxies derived data.  It never forwards Magpie credentials,
caller keys, arbitrary paths or request logs, and it only talks to Magpie on
loopback (see ``magpie_payload.MagpieClient``).

Bind it to the LAN address the device can reach and keep the optional bearer
token enabled on shared networks.  The token is read from an environment file
(``~/.config/magpie-display/env`` by default) and is never committed.
"""

from __future__ import annotations

import argparse
import hmac
import json
import os
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from magpie_payload import (
    DEFAULT_MAGPIE_URL,
    DEFAULT_POLL_INTERVAL_S,
    MagpieClient,
    Snapshot,
    SnapshotCache,
)

DEFAULT_BIND = "0.0.0.0"
DEFAULT_PORT = 8099
DEFAULT_ENV_FILE = Path("~/.config/magpie-display/env").expanduser()
TOKEN_ENV = "MAGPIE_DISPLAY_TOKEN"


def read_env_file(path: Path) -> dict[str, str]:
    """Parse a small ``KEY=VALUE`` file without executing anything."""
    values: dict[str, str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return values
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip("'\"")
    return values


def resolve_token(explicit: str | None, env_file: Path) -> str | None:
    if explicit:
        return explicit
    from_env = os.environ.get(TOKEN_ENV)
    if from_env:
        return from_env
    token = read_env_file(env_file).get(TOKEN_ENV)
    return token or None


class SnapshotServer:
    """Owns the polling thread and the last-good snapshot."""

    def __init__(
        self,
        cache: SnapshotCache,
        poll_interval_s: float = DEFAULT_POLL_INTERVAL_S,
    ) -> None:
        self.cache = cache
        self.poll_interval_s = max(1.0, poll_interval_s)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_success: float | None = None

    def start(self) -> Snapshot | None:
        snapshot, _ = self.cache.refresh()
        if snapshot is not None:
            self._last_success = time.monotonic()
            print(
                f"magpie-http: first snapshot with {snapshot.count} subscriptions",
                flush=True,
            )
        self._thread = threading.Thread(
            target=self._loop, name="magpie-poller", daemon=True
        )
        self._thread.start()
        return snapshot

    def _loop(self) -> None:
        while not self._stop.wait(self.poll_interval_s):
            snapshot, changed = self.cache.refresh()
            if snapshot is None:
                error = self.cache.error()
                print(f"magpie-http: refresh failed: {error}", file=sys.stderr,
                      flush=True)
                continue
            if changed:
                self._last_success = time.monotonic()
                print(
                    f"magpie-http: snapshot updated ({snapshot.count} subs)",
                    flush=True,
                )

    def age_s(self) -> float | None:
        if self._last_success is None:
            return None
        return time.monotonic() - self._last_success

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


class DisplayRequestHandler(BaseHTTPRequestHandler):
    server_version = "magpie-display/1"
    protocol_version = "HTTP/1.1"

    # Injected by make_server().
    cache: SnapshotCache
    token: str | None = None
    poller: SnapshotServer | None = None

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
        # Keep the journal useful without logging tokens or query strings.
        print(f"magpie-http: {self.address_string()} {format % args}",
              file=sys.stderr, flush=True)

    def _send(self, status: HTTPStatus, body: bytes,
              content_type: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _authorised(self) -> bool:
        if not self.token:
            return True
        header = self.headers.get("Authorization", "")
        prefix = "Bearer "
        if not header.startswith(prefix):
            return False
        return hmac.compare_digest(header[len(prefix):], self.token)

    def do_GET(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if not self._authorised():
            body = json.dumps({"error": "unauthorised"}).encode()
            self._send(HTTPStatus.UNAUTHORIZED, body)
            return

        snapshot: Snapshot | None = self.cache.get()
        if path == "/v1/display/health":
            if self.poller is None:
                age = None
            else:
                age = self.poller.age_s()
            body = json.dumps(
                {
                    "ok": snapshot is not None,
                    "t": snapshot.index["t"] if snapshot else 0,
                    "c": snapshot.count if snapshot else 0,
                    "age_s": round(age, 1) if age is not None else None,
                    "error": self.cache.error(),
                },
                separators=(",", ":"),
            ).encode()
            self._send(HTTPStatus.OK if snapshot else HTTPStatus.SERVICE_UNAVAILABLE,
                       body)
            return

        if snapshot is None:
            body = json.dumps(
                {"error": "no snapshot yet", "detail": self.cache.error()}
            ).encode()
            self._send(HTTPStatus.SERVICE_UNAVAILABLE, body)
            return

        if path == "/v1/display/index":
            self._send(HTTPStatus.OK, snapshot.index_bytes)
            return

        prefix = "/v1/display/sub/"
        if path.startswith(prefix):
            slot = path[len(prefix):]
            if not slot.isdigit():
                self._send(HTTPStatus.NOT_FOUND,
                           b'{"error":"bad slot"}')
                return
            index = int(slot)
            if index >= snapshot.count:
                self._send(HTTPStatus.NOT_FOUND,
                           b'{"error":"slot out of range"}')
                return
            self._send(HTTPStatus.OK, snapshot.sub_bytes[index])
            return

        self._send(HTTPStatus.NOT_FOUND, b'{"error":"not found"}')

    def do_POST(self) -> None:  # noqa: N802
        self._send(HTTPStatus.METHOD_NOT_ALLOWED, b'{"error":"read-only"}')

    def do_PUT(self) -> None:  # noqa: N802
        self._send(HTTPStatus.METHOD_NOT_ALLOWED, b'{"error":"read-only"}')


def make_server(
    bind: str,
    port: int,
    cache: SnapshotCache,
    token: str | None,
    poller: SnapshotServer,
) -> ThreadingHTTPServer:
    handler = type(
        "BoundDisplayRequestHandler",
        (DisplayRequestHandler,),
        {"cache": cache, "token": token, "poller": poller},
    )
    server = ThreadingHTTPServer((bind, port), handler)
    server.daemon_threads = True
    return server


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", default=os.environ.get("MAGPIE_DISPLAY_BIND",
                                                         DEFAULT_BIND),
                        help="LAN address to bind (default 0.0.0.0)")
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("MAGPIE_DISPLAY_PORT",
                                                   DEFAULT_PORT)))
    parser.add_argument("--magpie-url",
                        default=os.environ.get("MAGPIE_URL", DEFAULT_MAGPIE_URL))
    parser.add_argument("--poll-interval", type=float,
                        default=float(os.environ.get("MAGPIE_DISPLAY_POLL_S",
                                                     DEFAULT_POLL_INTERVAL_S)))
    parser.add_argument("--env-file", type=Path, default=DEFAULT_ENV_FILE)
    parser.add_argument("--token", help="bearer token; prefer the env file")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    token = resolve_token(args.token, args.env_file)
    cache = SnapshotCache(MagpieClient(args.magpie_url))
    poller = SnapshotServer(cache, args.poll_interval)
    poller.start()
    server = make_server(args.bind, args.port, cache, token, poller)
    print(
        f"magpie-http: listening on http://{args.bind}:{args.port} "
        f"(magpie={args.magpie_url}, token={'yes' if token else 'no'})",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        poller.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
