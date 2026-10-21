#!/usr/bin/env python3
"""Magpie usage payload builder shared by the BLE and HTTP companions.

The device (ESP-Mosaico Magpie usage display) is a *pull* client.  This module
turns the local Magpie ``/v1/magpie/quotas`` document into the two compact
payloads the device understands:

``INDEX``
    ``{"v":1,"t":<epoch>,"c":<count>,"lo":{...},"ids":[0..c-1]}``

``SUB``
    ``{"i":<slot>,"p":<provider>,"n":<name>,"pl":<plan>,"e":<0|1>,
       "w":[{"n":"5h","u":84,"r":<epoch>}, ... up to 3]}``

Design constraints (see ``docs/protocol.md`` in the application):

* One subscription per device page; at most 16 subscriptions.
* At most 3 windows per subscription, ordered most-used first.
* Every encoded payload must fit ``SUB_PAYLOAD_MAX`` bytes so it works with a
  185-byte ATT MTU without long reads.
* Only derived data leaves the machine.  Magpie credentials, caller keys,
  request logs and arbitrary paths are never forwarded.

This module is pure standard library, so the HTTP server, the BLE server and
the tests all share it without a third-party dependency.
"""

from __future__ import annotations

import json
import math
import re
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

PROTOCOL_VERSION = 1
MAX_SUBSCRIPTIONS = 16
MAX_WINDOWS = 3
SUB_PAYLOAD_MAX = 180
INDEX_PAYLOAD_MAX = 180

PROVIDER_MAX = 24
NAME_MAX = 24
PLAN_MAX = 12
WINDOW_LABEL_MAX = 6

DEFAULT_MAGPIE_URL = "http://127.0.0.1:3425"
DEFAULT_QUOTAS_TIMEOUT_S = 30.0
DEFAULT_POLL_INTERVAL_S = 15.0

_ISO_PATTERN = re.compile(
    r"^(?P<year>\d{4})-(?P<month>\d{2})-(?P<day>\d{2})"
    r"(?:[T ](?P<hour>\d{2}):(?P<minute>\d{2})"
    r"(?::(?P<second>\d{2})(?:\.\d+)?)?"
    r"(?P<tz>Z|[+-]\d{2}:\d{2})?)?$"
)

# Magpie window names we have seen, mapped to the short labels the device can
# afford inside a 180-byte payload.
_WINDOW_LABELS = {
    "5 hours": "5h",
    "5 hour": "5h",
    "24 hours": "24h",
    "1 day": "1d",
    "7 days": "7d",
    "weekly": "7d",
    "daily": "1d",
    "monthly": "30d",
    "chat requests": "chat",
    "completions": "comp",
    "cursor models": "models",
    "other models": "other",
    "on-demand": "od",
    "total": "tot",
    "grok bot": "grok",
    "credits": "cred",
    "balance": "bal",
}

_NON_PRINTABLE = re.compile(r"[^\x20-\x7e]")
# "Claude Code · Remote magpie" -> "Claude Code"; the suffix is gateway noise
# and costs wire bytes on every subscription.
_REMOTE_SUFFIX = re.compile(r"\s*[\u00b7\u2022|]\s*.*$")


class MagpieError(RuntimeError):
    """Magpie could not be read or returned an unexpected document."""


def ascii_text(value: Any, limit: int) -> str:
    """ASCII-only, length-limited text that is safe for the device font."""
    if value is None:
        return ""
    text = _NON_PRINTABLE.sub("?", str(value)).strip()
    return text[:limit]


def encode(payload: dict[str, Any]) -> bytes:
    """Encode a payload exactly as it goes on the wire."""
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode(
        "ascii"
    )


def short_provider(provider: Any) -> str:
    """``office/claude`` -> ``claude`` (the device shows the leaf only)."""
    text = ascii_text(provider, PROVIDER_MAX)
    leaf = text.rsplit("/", 1)[-1] if text else ""
    return leaf or text or "?"


def short_window_label(name: Any) -> str:
    text = ascii_text(name, 24).lower()
    mapped = _WINDOW_LABELS.get(text)
    if mapped:
        return mapped
    token = re.split(r"[^a-z0-9]+", text, maxsplit=1)[0]
    return (token or "win")[:WINDOW_LABEL_MAX]


def parse_iso_epoch(value: Any) -> int:
    """ISO-8601 (``Z``, ``+08:00``, fractional seconds) -> Unix seconds.

    Returns 0 for ``None`` or an unparsable value; the device renders 0 as an
    unknown reset time.
    """
    if not isinstance(value, str):
        return 0
    match = _ISO_PATTERN.match(value.strip())
    if not match:
        return 0
    parts = match.groupdict()
    try:
        moment = datetime(
            int(parts["year"]),
            int(parts["month"]),
            int(parts["day"]),
            int(parts["hour"] or 0),
            int(parts["minute"] or 0),
            int(parts["second"] or 0),
            tzinfo=timezone.utc,
        )
    except ValueError:
        return 0
    tz = parts["tz"]
    offset = 0
    if tz and tz != "Z":
        sign = -1 if tz[0] == "-" else 1
        offset = sign * (int(tz[1:3]) * 3600 + int(tz[4:6]) * 60)
    return int(moment.timestamp()) - offset


def used_percent(window: dict[str, Any]) -> int | None:
    """Integer used percent for one Magpie window, or None when unusable."""
    if window.get("unlimited") is True:
        return None
    used = window.get("used")
    if isinstance(used, (int, float)) and not isinstance(used, bool):
        value = float(used)
    else:
        remaining = window.get("remaining")
        if not isinstance(remaining, (int, float)) or isinstance(
            remaining, bool
        ):
            return None
        value = 100.0 - float(remaining)
    if not math.isfinite(value):
        return None
    return int(min(100.0, max(0.0, value)) + 0.5)


def window_label(window: dict[str, Any]) -> str:
    return short_window_label(window.get("name") or "win")


def derive_subscription(entry: dict[str, Any]) -> dict[str, Any]:
    """One Magpie provider entry -> the full (untruncated) device subscription."""
    provider = short_provider(entry.get("provider"))
    raw_name = _REMOTE_SUFFIX.sub("", str(entry.get("name") or ""))
    name = ascii_text(raw_name or provider, NAME_MAX)
    plan = ascii_text(entry.get("plan"), PLAN_MAX)
    error = bool(entry.get("error"))

    # Keep the windows that actually show usage; a provider with only unused
    # windows still shows its first one so the page is not empty.
    windows = [
        window
        for window in derive_subscription_windows(entry)
        if window["u"] > 0
    ][:MAX_WINDOWS]
    if not windows:
        windows = derive_subscription_windows(entry)[:1]
    return {
        "p": provider,
        "n": name,
        "pl": plan,
        "e": 1 if error else 0,
        "w": windows,
    }


def derive_subscription_windows(entry: dict[str, Any]) -> list[dict[str, Any]]:
    """Usable windows of one provider, most used first."""
    windows: list[dict[str, Any]] = []
    for window in entry.get("windows") or []:
        if not isinstance(window, dict):
            continue
        used = used_percent(window)
        if used is None:
            continue
        windows.append(
            {
                "n": window_label(window),
                "u": used,
                "r": parse_iso_epoch(window.get("resetsAt")),
            }
        )
    windows.sort(key=lambda item: (-item["u"], item["n"]))
    return windows


def _dedupe_names(subscriptions: list[dict[str, Any]]) -> None:
    """Two Claude accounts share a name; number them in provider order."""
    seen: dict[str, int] = {}
    totals: dict[str, int] = {}
    for sub in subscriptions:
        totals[sub["p"]] = totals.get(sub["p"], 0) + 1
    for sub in subscriptions:
        provider = sub["p"]
        if totals[provider] > 1:
            seen[provider] = seen.get(provider, 0) + 1
            sub["n"] = f"{sub['n']} ({seen[provider]})"


def _order_key(sub: dict[str, Any]) -> tuple[Any, ...]:
    worst = max((window["u"] for window in sub["w"]), default=-1)
    return (
        1 if sub["e"] else 0,       # signed-out providers last
        0 if sub["w"] else 1,       # balance-only / no windows after quotas
        -worst,                     # most constrained first
        sub["n"].casefold(),
    )


def derive_subscriptions(
    quotas: dict[str, Any], limit: int = MAX_SUBSCRIPTIONS
) -> list[dict[str, Any]]:
    """Magpie quotas document -> ordered, deduped, display-ready subscriptions."""
    data = quotas.get("data")
    if not isinstance(data, list):
        raise MagpieError("Magpie quotas response has no data array")
    subscriptions: list[dict[str, Any]] = []
    for entry in data:
        if not isinstance(entry, dict):
            continue
        subscriptions.append(derive_subscription(entry))
    subscriptions.sort(key=_order_key)
    subscriptions = subscriptions[:limit]
    _dedupe_names(subscriptions)
    for index, sub in enumerate(subscriptions):
        sub["i"] = index
    return subscriptions


def _fit_subscription(sub: dict[str, Any]) -> tuple[dict[str, Any], bytes]:
    """Shrink one subscription until it fits ``SUB_PAYLOAD_MAX`` bytes."""
    candidate = {
        "i": sub["i"],
        "p": sub["p"],
        "n": sub["n"],
        "pl": sub["pl"],
        "e": sub["e"],
        "w": [dict(window) for window in sub["w"]],
    }
    while True:
        blob = encode(candidate)
        if len(blob) <= SUB_PAYLOAD_MAX:
            return candidate, blob
        if candidate["w"]:
            candidate["w"].pop()
        elif candidate["pl"]:
            candidate["pl"] = candidate["pl"][:-1]
        elif len(candidate["n"]) > 6:
            candidate["n"] = candidate["n"][:-1]
        elif len(candidate["p"]) > 6:
            candidate["p"] = candidate["p"][:-1]
        else:
            raise MagpieError(
                f"subscription cannot fit {SUB_PAYLOAD_MAX} bytes: {blob!r}"
            )


def build_sub_payloads(
    subscriptions: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[bytes]]:
    fitted: list[dict[str, Any]] = []
    blobs: list[bytes] = []
    for sub in subscriptions:
        compact, blob = _fit_subscription(sub)
        fitted.append(compact)
        blobs.append(blob)
    return fitted, blobs


def build_index_payload(
    subscriptions: list[dict[str, Any]], now: int
) -> tuple[dict[str, Any], bytes]:
    """INDEX payload: version, sender time, count, glance summary, slot ids."""
    worst: dict[str, Any] | None = None
    worst_used = -1
    for sub in subscriptions:
        for window in sub["w"]:
            if window["u"] > worst_used:
                worst_used = window["u"]
                worst = {
                    "i": sub["i"],
                    "n": sub["n"],
                    "w": window["n"],
                    "u": window["u"],
                    "r": window["r"],
                }
    index = {
        "v": PROTOCOL_VERSION,
        "t": int(now),
        "c": len(subscriptions),
        "lo": worst or {},
        "ids": list(range(len(subscriptions))),
    }
    blob = encode(index)
    if len(blob) > INDEX_PAYLOAD_MAX:
        raise MagpieError(
            f"index cannot fit {INDEX_PAYLOAD_MAX} bytes: {blob!r}"
        )
    return index, blob


@dataclass
class Snapshot:
    """One derived, wire-ready Magpie snapshot."""

    index: dict[str, Any]
    index_bytes: bytes
    subs: list[dict[str, Any]] = field(default_factory=list)
    sub_bytes: list[bytes] = field(default_factory=list)
    revision_key: tuple[bytes, bytes] = (b"", b"")

    @property
    def count(self) -> int:
        return self.index["c"]


def build_snapshot(quotas: dict[str, Any], now: int | None = None) -> Snapshot:
    """Full derivation: quotas document -> INDEX and SUB payloads."""
    if now is None:
        now = int(datetime.now(tz=timezone.utc).timestamp())
    subscriptions = derive_subscriptions(quotas)
    fitted, sub_bytes = build_sub_payloads(subscriptions)
    index, index_bytes = build_index_payload(fitted, now)
    # The sender timestamp alone must not force a BLE notification: the data
    # revision ignores "t" and covers every subscription payload.
    revision_key = (
        encode({key: value for key, value in index.items() if key != "t"}),
        b"".join(sub_bytes),
    )
    return Snapshot(
        index=index,
        index_bytes=index_bytes,
        subs=fitted,
        sub_bytes=sub_bytes,
        revision_key=revision_key,
    )


# ---------------------------------------------------------------------------
# Magpie access (loopback only by default)
# ---------------------------------------------------------------------------


class MagpieClient:
    """Minimal read-only Magpie client.

    Magpie trusts loopback callers, so no credential is needed with the default
    ``http://127.0.0.1:3425``.  A non-loopback Magpie needs a caller key, which
    this project deliberately does not handle: point the client at loopback.
    """

    def __init__(
        self,
        base_url: str = DEFAULT_MAGPIE_URL,
        quotas_timeout_s: float = DEFAULT_QUOTAS_TIMEOUT_S,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.quotas_timeout_s = quotas_timeout_s

    def _get_json(self, path: str, timeout: float) -> Any:
        request = urllib.request.Request(
            self.base_url + path, headers={"Accept": "application/json"}
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                if response.status != 200:
                    raise MagpieError(
                        f"GET {path} returned HTTP {response.status}"
                    )
                body = response.read()
        except urllib.error.HTTPError as error:
            raise MagpieError(f"GET {path} returned HTTP {error.code}") from error
        except urllib.error.URLError as error:
            raise MagpieError(f"GET {path} failed: {error.reason}") from error
        except TimeoutError as error:
            raise MagpieError(f"GET {path} timed out") from error
        try:
            return json.loads(body)
        except json.JSONDecodeError as error:
            raise MagpieError(f"GET {path} returned invalid JSON") from error

    def health(self) -> dict[str, Any]:
        document = self._get_json("/", timeout=10)
        if not isinstance(document, dict) or document.get("name") != "magpie":
            raise MagpieError("endpoint is not a Magpie gateway")
        return document

    def quotas(self) -> dict[str, Any]:
        # Cold Magpie can take more than five seconds; use the documented 30 s.
        return self._get_json("/v1/magpie/quotas", self.quotas_timeout_s)

    def snapshot(self, now: int | None = None) -> Snapshot:
        return build_snapshot(self.quotas(), now=now)


class SnapshotCache:
    """Thread-safe last-good snapshot shared by the BLE and HTTP servers."""

    def __init__(self, client: MagpieClient) -> None:
        self.client = client
        self._lock = threading.Lock()
        self._snapshot: Snapshot | None = None
        self._error: str | None = None
        self._revision: tuple[bytes, bytes] | None = None

    def refresh(self) -> tuple[Snapshot | None, bool]:
        """Fetch and rebuild. Returns (snapshot, changed)."""
        try:
            snapshot = self.client.snapshot()
        except MagpieError as error:
            with self._lock:
                self._error = str(error)
            return None, False
        with self._lock:
            changed = self._revision != snapshot.revision_key
            self._snapshot = snapshot
            self._revision = snapshot.revision_key
            self._error = None
        return snapshot, changed

    def get(self) -> Snapshot | None:
        with self._lock:
            return self._snapshot

    def error(self) -> str | None:
        with self._lock:
            return self._error
