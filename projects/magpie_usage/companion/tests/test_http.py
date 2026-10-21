#!/usr/bin/env python3
"""Loopback tests for the read-only HTTP fallback server."""

from __future__ import annotations

import json
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPANION = HERE.parent
PROJECT = COMPANION.parent

sys.path.insert(0, str(COMPANION))

import magpie_http_server as http_server  # noqa: E402
import magpie_payload as mp  # noqa: E402

QUOTAS = json.loads(
    (PROJECT / "tests" / "host" / "fixtures" / "quotas.json").read_text()
)


class FakeMagpie(BaseHTTPRequestHandler):
    """Tiny loopback stand-in for the local Magpie gateway."""

    protocol_version = "HTTP/1.1"
    quotas_requests = 0

    def log_message(self, *args):  # silence
        pass

    def _send(self, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        if self.path == "/":
            self._send({"name": "magpie", "version": "test"})
        elif self.path == "/v1/magpie/quotas":
            FakeMagpie.quotas_requests += 1
            self._send(QUOTAS)
        else:
            self.send_error(404)


def start_fake_magpie() -> tuple[ThreadingHTTPServer, str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeMagpie)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, args=(0.05,),
                              daemon=True)
    thread.start()
    host, port = server.server_address
    return server, f"http://{host}:{port}"


def start_display(token: str | None = None):
    server, magpie_url = start_fake_magpie()
    cache = mp.SnapshotCache(mp.MagpieClient(magpie_url))
    poller = http_server.SnapshotServer(cache, poll_interval_s=60)
    poller.start()
    display = http_server.make_server("127.0.0.1", 0, cache, token, poller)
    thread = threading.Thread(target=display.serve_forever, args=(0.05,),
                              daemon=True)
    thread.start()
    host, port = display.server_address
    return server, display, poller, f"http://{host}:{port}"


def fetch(url: str, headers: dict | None = None):
    request = urllib.request.Request(url, headers={"Connection": "close",
                                                   **(headers or {})})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


class HttpEndpointTests(unittest.TestCase):
    def setUp(self):
        (self.magpie, self.display, self.poller,
         self.base_url) = start_display()

    def tearDown(self):
        self.display.shutdown()
        self.display.server_close()
        self.magpie.shutdown()
        self.magpie.server_close()
        self.poller.stop()

    def test_index_and_subs(self):
        status, body = fetch(self.base_url + "/v1/display/index")
        self.assertEqual(status, 200)
        index = json.loads(body)
        self.assertEqual(index["v"], 1)
        self.assertEqual(index["c"], 7)
        self.assertEqual(index["ids"], list(range(7)))
        self.assertLessEqual(len(body), mp.INDEX_PAYLOAD_MAX)

        for slot in range(index["c"]):
            status, body = fetch(self.base_url + f"/v1/display/sub/{slot}")
            self.assertEqual(status, 200)
            sub = json.loads(body)
            self.assertEqual(sub["i"], slot)
            self.assertLessEqual(len(body), mp.SUB_PAYLOAD_MAX)

    def test_health(self):
        status, body = fetch(self.base_url + "/v1/display/health")
        self.assertEqual(status, 200)
        health = json.loads(body)
        self.assertTrue(health["ok"])
        self.assertEqual(health["c"], 7)
        self.assertIsNotNone(health["age_s"])

    def test_errors(self):
        self.assertEqual(fetch(self.base_url + "/v1/display/sub/7")[0], 404)
        self.assertEqual(fetch(self.base_url + "/v1/display/sub/x")[0], 404)
        self.assertEqual(fetch(self.base_url + "/v1/display/other")[0], 404)
        request = urllib.request.Request(self.base_url + "/v1/display/index",
                                         data=b"{}", method="POST")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=5)
        self.assertEqual(caught.exception.code, 405)

    def test_never_forwards_arbitrary_paths(self):
        # The server has no proxy route: a Magpie path is simply 404.
        status, _ = fetch(self.base_url + "/v1/magpie/quotas")
        self.assertEqual(status, 404)


class TokenTests(unittest.TestCase):
    def setUp(self):
        (self.magpie, self.display, self.poller,
         self.base_url) = start_display(token="s3cret")

    def tearDown(self):
        self.display.shutdown()
        self.display.server_close()
        self.magpie.shutdown()
        self.magpie.server_close()
        self.poller.stop()

    def test_token_required(self):
        self.assertEqual(fetch(self.base_url + "/v1/display/index")[0], 401)
        self.assertEqual(
            fetch(self.base_url + "/v1/display/index",
                  {"Authorization": "Bearer wrong"})[0],
            401,
        )
        status, body = fetch(self.base_url + "/v1/display/index",
                             {"Authorization": "Bearer s3cret"})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["c"], 7)


class EnvFileTests(unittest.TestCase):
    def test_parses_quotes_and_comments(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "env"
            path.write_text(
                "# comment\n"
                "MAGPIE_DISPLAY_TOKEN=\"abc123\"\n"
                "OTHER=1\n"
                "invalid line\n"
            )
            values = http_server.read_env_file(path)
            self.assertEqual(values, {"MAGPIE_DISPLAY_TOKEN": "abc123",
                                      "OTHER": "1"})
            self.assertEqual(
                http_server.resolve_token("explicit", path), "explicit"
            )

    def test_missing_file(self):
        missing = Path("/nonexistent/magpie-display/env")
        self.assertEqual(http_server.read_env_file(missing), {})
        self.assertIsNone(http_server.resolve_token(None, missing))


if __name__ == "__main__":
    unittest.main()
