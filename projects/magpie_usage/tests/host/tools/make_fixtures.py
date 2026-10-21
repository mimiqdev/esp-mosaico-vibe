#!/usr/bin/env python3
"""Capture Magpie API responses and rewrite them into safe test fixtures.

Run against a *read-only* gateway:

    python3 tools/make_fixtures.py --base-url http://127.0.0.1:3425 \
        --out fixtures

The script only issues GET requests. It strips account identifiers (e-mail,
user names, gateway keys, request ids, sessions, hosts) and truncates the
30-day series so the fixtures stay small and contain no personal data.

The fixtures keep the shapes that matter for the parsers:
fractional `remaining`, `+08:00` and `Z` timestamps, `resetsAt: null`,
providers with `error` and no windows, and an `unlimited` window.
"""

from __future__ import annotations

import argparse
import json
import re
import urllib.request
from pathlib import Path
from typing import Any

EMAIL = re.compile(r"[\w.+-]+@[\w-]+\.[\w.]+")
TAILNET_HOST = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}:\d+\b")
SECRET = re.compile(r"sk-[A-Za-z0-9_-]{8,}")


def scrub(value: Any) -> Any:
    """Recursively replace identifying text with stable placeholders."""
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in {"user", "email", "account"}:
                result[key] = "user@example.com"
            elif key in {"rid", "response_id", "session", "native_session"}:
                result[key] = "00000000-0000-0000-0000-000000000000"
            elif key in {"host", "callerKey", "callerKeys", "providerKeyId"}:
                continue
            elif key in {"cost", "price"}:
                result[key] = round(float(item or 0), 6) if item is not None else 0
            else:
                result[key] = scrub(item)
        return result
    if isinstance(value, list):
        return [scrub(item) for item in value]
    if isinstance(value, str):
        text = EMAIL.sub("user@example.com", value)
        text = TAILNET_HOST.sub("gateway.invalid:3425", text)
        text = SECRET.sub("sk-removed", text)
        return text
    return value


def fetch(base_url: str, path: str) -> Any:
    request = urllib.request.Request(base_url.rstrip("/") + path,
                                     headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def keep_providers(payload: dict) -> dict:
    """Keep a representative subset: worst window, reset formats, errors."""
    wanted = {"office/claude", "office/codex", "office/cursor",
              "office/kimi-code-cn", "office/workbuddy", "copilot"}
    providers = [entry for entry in payload.get("data", [])
                 if entry.get("provider") in wanted]
    for entry in providers:
        entry.pop("holdsWhy", None)
        for window in entry.get("windows") or []:
            window.pop("holdsWhy", None)
            window.pop("holds", None)
    payload["data"] = providers
    payload["object"] = "list"
    return payload


USAGE_TOTALS = ("period", "since", "calls", "errors", "retried", "timed",
                "input", "output", "cache_read", "cache_write", "reasoning",
                "cost", "unpriced", "ttft_ms", "decode_ms", "decode_n",
                "decode_out", "bucket")
SERIES_FIELDS = ("label", "time", "calls", "errors", "input", "output",
                 "cache_read", "cache_write", "reasoning", "cost")
REQUEST_FIELDS = ("t", "agent", "agentName", "provider", "providerName",
                  "model", "req", "served", "in", "out", "cache_read",
                  "cache_write", "reasoning", "effort", "ms", "ttft_ms",
                  "status", "ep", "stop", "cost", "priced")


def trim_usage(payload: dict) -> dict:
    """Keep the aggregate block and the last seven daily buckets.

    A whitelist keeps per-agent, per-model and per-session breakdowns (which
    carry session ids) out of the fixtures.
    """
    trimmed = {key: payload[key] for key in USAGE_TOTALS if key in payload}
    series = [{key: bucket[key] for key in SERIES_FIELDS if key in bucket}
              for bucket in (payload.get("series") or [])[-7:]]
    trimmed["series"] = series
    return trimmed


def trim_requests(payload: dict) -> dict:
    rows = [{key: row[key] for key in REQUEST_FIELDS if key in row}
            for row in (payload.get("rows") or [])[:2]]
    return {
        "period": payload.get("period"),
        "rows": rows,
        "offset": payload.get("offset", 0),
        "total": payload.get("total", len(rows)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:3425")
    parser.add_argument("--out", type=Path, default=Path("fixtures"))
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    documents = {
        "health.json": scrub(fetch(args.base_url, "/")),
        "quotas.json": scrub(
            keep_providers(fetch(args.base_url, "/v1/magpie/quotas"))),
        "usage.json": scrub(trim_usage(fetch(args.base_url, "/v1/magpie/usage"))),
        "requests.json": scrub(
            trim_requests(fetch(args.base_url, "/v1/magpie/usage/requests?limit=2"))),
    }
    for name, document in documents.items():
        path = args.out / name
        path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
        print(f"wrote {path} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
