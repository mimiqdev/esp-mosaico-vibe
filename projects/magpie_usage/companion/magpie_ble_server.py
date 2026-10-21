#!/usr/bin/env python3
"""Magpie usage BLE peripheral (BlueZ GATT, dbus-next).

Pulls the local Magpie quotas every 15 s, derives the compact payloads with
``magpie_payload`` and publishes them over the GATT profile implemented in
``magpie_bluez``.  The device is a BLE *central*: it scans for the service,
subscribes to INDEX and reads one SUB payload per page.

Run it on the omarchy host:

    ~/.venvs/magpie-display/bin/python magpie_ble_server.py --adapter /org/bluez/hci0

``dbus-next`` is required (``python -m pip install dbus-next``); it is not part
of the standard library.  BlueZ 5.87 is assumed, with the default D-Bus policy
that lets the user session call ``org.bluez``.  The adapter must be powered.

A dry run without touching D-Bus exercises the payload/state path:

    python3 magpie_ble_server.py --dry-run --once
"""

from __future__ import annotations

import argparse
import asyncio
import signal
import sys
import time
from typing import Any

from magpie_payload import (
    DEFAULT_MAGPIE_URL,
    DEFAULT_POLL_INTERVAL_S,
    MAX_SUBSCRIPTIONS,
    MagpieClient,
    Snapshot,
    SnapshotCache,
    encode,
)

EMPTY_INDEX = {"v": 1, "t": 0, "c": 0, "lo": {}, "ids": []}


class GattModel:
    """Pure GATT state: index payload, per-slot SUB payloads and selection.

    Separated from the D-Bus interfaces so it can be tested without BlueZ.
    """

    def __init__(self, max_subs: int = MAX_SUBSCRIPTIONS) -> None:
        self.max_subs = max_subs
        self.index_bytes = encode(EMPTY_INDEX)
        self.sub_bytes: list[bytes] = []
        self.selected = 0
        self._revision: tuple[bytes, bytes] | None = None

    def apply(self, snapshot: Snapshot) -> bool:
        """Install a new snapshot; True when the data revision changed.

        The sender timestamp is not part of the revision, so a poll that finds
        the same usage does not wake subscribed centrals.
        """
        changed = self._revision != snapshot.revision_key
        self.index_bytes = snapshot.index_bytes
        self.sub_bytes = list(snapshot.sub_bytes[: self.max_subs])
        self._revision = snapshot.revision_key
        if self.selected >= len(self.sub_bytes):
            self.selected = 0
        return changed

    def read_index(self) -> bytes:
        return self.index_bytes

    def write_selection(self, value: bytes) -> None:
        """SUB write: one byte slot id in 0..count-1."""
        if len(value) != 1:
            raise ValueError(
                f"SUB expects exactly one slot byte, got {len(value)}"
            )
        slot = value[0]
        if slot >= len(self.sub_bytes):
            raise ValueError(f"slot {slot} is outside 0..{len(self.sub_bytes) - 1}")
        self.selected = slot

    def read_sub(self) -> bytes:
        if not self.sub_bytes:
            return b"{}"
        return self.sub_bytes[self.selected]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--magpie-url", default=DEFAULT_MAGPIE_URL)
    parser.add_argument("--poll-interval", type=float,
                        default=DEFAULT_POLL_INTERVAL_S)
    parser.add_argument("--adapter", default="/org/bluez/hci0")
    parser.add_argument("--encrypt", action="store_true",
                        help="require bonding (encrypt-read/notify) for reads")
    parser.add_argument("--dry-run", action="store_true",
                        help="derive payloads without touching D-Bus")
    parser.add_argument("--once", action="store_true",
                        help="publish one snapshot and exit")
    return parser


def describe(model: GattModel) -> None:
    print(f"index ({len(model.index_bytes)} B): {model.index_bytes.decode()}")
    for slot, blob in enumerate(model.sub_bytes):
        print(f"sub {slot} ({len(blob)} B): {blob.decode()}")


async def run(args: argparse.Namespace) -> int:
    cache = SnapshotCache(MagpieClient(args.magpie_url))
    model = GattModel()
    peripheral = None
    if not args.dry_run:
        from magpie_bluez import BlueZPeripheral

        peripheral = BlueZPeripheral(model, adapter=args.adapter,
                                     encrypted=args.encrypt)
        await peripheral.start()
        print(f"magpie-ble: advertising as 'magpie-usage' on {args.adapter}",
              flush=True)

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:  # non-Unix
            pass

    period = max(1.0, args.poll_interval)
    last_index = b""
    while not stop.is_set():
        snapshot, changed = cache.refresh()
        if snapshot is None:
            print(f"magpie-ble: refresh failed: {cache.error()}", file=sys.stderr,
                  flush=True)
        else:
            model.apply(snapshot)
            if model.index_bytes != last_index:
                last_index = model.index_bytes
                if args.dry_run:
                    print(
                        f"magpie-ble: snapshot {snapshot.index['t']} "
                        f"with {snapshot.count} subscriptions",
                        flush=True,
                    )
                    if args.once:
                        describe(model)
                if peripheral is not None:
                    await peripheral.publish(model.index_bytes, changed)
        if args.once:
            break
        try:
            await asyncio.wait_for(stop.wait(), timeout=period)
        except asyncio.TimeoutError:
            pass

    if peripheral is not None:
        await peripheral.stop()
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return asyncio.run(run(args))
    except Exception as error:
        print(f"magpie-ble: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
