#!/usr/bin/env python3
"""Regenerate the protocol payload fixtures from the captured Magpie document.

The device decodes exactly these bytes in the host tests, so the fixture set is
produced by the same companion builder that the omarchy servers use:

    python3 tests/host/tools/make_payloads.py

Output: tests/host/fixtures/payloads/{index.json,sub-0.json,...}
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[2]
FIXTURES = PROJECT / "tests" / "host" / "fixtures"
OUTPUT = FIXTURES / "payloads"

sys.path.insert(0, str(PROJECT / "companion"))

import magpie_payload as mp  # noqa: E402

# 2026-10-21T11:30:00+08:00, the capture time of quotas.json.
NOW = 1792543800


def main() -> int:
    quotas = json.loads((FIXTURES / "quotas.json").read_text())
    snapshot = mp.build_snapshot(quotas, now=NOW)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    (OUTPUT / "index.json").write_text(snapshot.index_bytes.decode() + "\n")
    for slot, blob in enumerate(snapshot.sub_bytes):
        (OUTPUT / f"sub-{slot}.json").write_text(blob.decode() + "\n")
    # Remove leftovers when the subscription count shrinks.
    for stale in OUTPUT.glob("sub-*.json"):
        slot = int(stale.stem.split("-")[1])
        if slot >= snapshot.count:
            stale.unlink()
    print(
        f"wrote {snapshot.count} subscriptions "
        f"(index {len(snapshot.index_bytes)} B, "
        f"max sub {max(len(b) for b in snapshot.sub_bytes)} B) "
        f"to {OUTPUT}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
