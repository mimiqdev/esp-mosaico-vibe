#!/usr/bin/env python3
"""Tests for the shared payload builder (standard library only)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPANION = HERE.parent
PROJECT = COMPANION.parent

sys.path.insert(0, str(COMPANION))

import magpie_payload as mp  # noqa: E402

FIXTURES = PROJECT / "tests" / "host" / "fixtures"
# 2026-10-21T11:30:00+08:00, the fixture capture time.
NOW = 1792543800


def load_quotas(name: str = "quotas.json") -> dict:
    return json.loads((FIXTURES / name).read_text())


def provider(**overrides) -> dict:
    entry = {
        "provider": "office/example",
        "name": "Example · Remote magpie",
        "plan": "Pro",
        "windows": [
            {
                "name": "5 hours",
                "used": 84,
                "remaining": 16,
                "resetsAt": "2026-10-21T12:29:00+08:00",
            }
        ],
    }
    entry.update(overrides)
    return entry


class IsoTests(unittest.TestCase):
    def test_offsets_and_fractions(self):
        self.assertEqual(
            mp.parse_iso_epoch("2026-01-01T00:00:00Z"), 1767225600
        )
        self.assertEqual(
            mp.parse_iso_epoch("2026-10-21T12:29:00+08:00"),
            mp.parse_iso_epoch("2026-10-21T04:29:00Z"),
        )
        self.assertEqual(
            mp.parse_iso_epoch("2026-10-21T04:00:26.052876Z"),
            mp.parse_iso_epoch("2026-10-21T04:00:26Z"),
        )
        self.assertEqual(mp.parse_iso_epoch(None), 0)
        self.assertEqual(mp.parse_iso_epoch("not a date"), 0)
        self.assertEqual(mp.parse_iso_epoch("2026-13-01T00:00:00Z"), 0)


class DeriveTests(unittest.TestCase):
    def test_fixture_order_and_dedup(self):
        subs = mp.derive_subscriptions(load_quotas())
        self.assertEqual(len(subs), 7)
        self.assertEqual([sub["i"] for sub in subs], list(range(7)))
        # Most constrained subscription first: Kimi's weekly window is at 100%.
        self.assertEqual(subs[0]["p"], "kimi-code-cn")
        # Two Claude rows keep their identity and get numbered names.
        claude = [sub for sub in subs if sub["p"] == "claude"]
        self.assertEqual(len(claude), 2)
        self.assertEqual({sub["n"] for sub in claude},
                         {"Claude Code (1)", "Claude Code (2)"})
        # The gateway's "· Remote magpie" suffix is stripped.
        for sub in subs:
            self.assertNotIn("Remote magpie", sub["n"])
        # Windows are most-used first and at most three.
        for sub in subs:
            self.assertLessEqual(len(sub["w"]), mp.MAX_WINDOWS)
            used = [window["u"] for window in sub["w"]]
            self.assertEqual(used, sorted(used, reverse=True))

    def test_zero_used_windows_are_dropped(self):
        subs = mp.derive_subscriptions(
            {
                "data": [
                    provider(
                        windows=[
                            {"name": "5 hours", "used": 0, "remaining": 100},
                            {"name": "7 days", "used": 42, "remaining": 58},
                        ]
                    )
                ]
            }
        )
        self.assertEqual([window["n"] for window in subs[0]["w"]], ["7d"])

    def test_unused_provider_keeps_one_window(self):
        subs = mp.derive_subscriptions(
            {"data": [provider(windows=[{"name": "Credits", "used": 0}])]}
        )
        self.assertEqual(subs[0]["w"][0]["n"], "cred")
        self.assertEqual(subs[0]["w"][0]["r"], 0)

    def test_remaining_only_and_clamping(self):
        subs = mp.derive_subscriptions(
            {
                "data": [
                    provider(
                        windows=[
                            {"name": "a", "remaining": 12.5},
                            {"name": "b", "remaining": 150},
                            {"name": "c", "remaining": -20},
                            {"name": "d", "remaining": None},
                        ]
                    )
                ]
            }
        )
        windows = {window["n"]: window["u"] for window in subs[0]["w"]}
        self.assertEqual(windows["a"], 88)
        self.assertNotIn("d", windows)  # unknown percent is skipped

    def test_unlimited_and_error(self):
        subs = mp.derive_subscriptions(
            {
                "data": [
                    provider(provider="office/err", name="Broken",
                             error="signed out", windows=[]),
                    provider(windows=[
                        {"name": "Limit", "remaining": 100, "unlimited": True},
                    ]),
                ]
            }
        )
        self.assertEqual(subs[0]["e"], 0)
        self.assertEqual(subs[0]["w"], [])
        self.assertEqual(subs[1]["e"], 1)
        self.assertEqual(subs[1]["p"], "err")

    def test_empty_and_bad_documents(self):
        subs = mp.derive_subscriptions({"data": []})
        self.assertEqual(subs, [])
        with self.assertRaises(mp.MagpieError):
            mp.derive_subscriptions({})

    def test_non_ascii_is_sanitised(self):
        subs = mp.derive_subscriptions(
            {"data": [provider(name="Mödel \"x\" 名字")]}
        )
        self.assertTrue(subs[0]["n"].isascii())
        self.assertNotIn("\n", subs[0]["n"])

    def test_cap_at_16(self):
        data = [
            provider(provider=f"p/{index}", name=f"Provider {index}",
                     windows=[{"name": "7 days", "used": index}])
            for index in range(20)
        ]
        snapshot = mp.build_snapshot({"data": data}, now=NOW)
        self.assertEqual(snapshot.count, mp.MAX_SUBSCRIPTIONS)
        self.assertEqual(snapshot.index["ids"], list(range(16)))


class PayloadSizeTests(unittest.TestCase):
    def test_every_fixture_subscription_fits(self):
        snapshot = mp.build_snapshot(load_quotas(), now=NOW)
        self.assertLessEqual(len(snapshot.index_bytes), mp.INDEX_PAYLOAD_MAX)
        self.assertEqual(len(snapshot.sub_bytes), snapshot.count)
        for blob in snapshot.sub_bytes:
            self.assertLessEqual(len(blob), mp.SUB_PAYLOAD_MAX)
            payload = json.loads(blob)
            self.assertEqual(payload["i"], snapshot.sub_bytes.index(blob))
            self.assertIn("w", payload)

    def test_worst_case_names_fit(self):
        data = [
            provider(
                provider="office/" + "very-long-provider-name-" + str(index),
                name="A very long subscription display name " + str(index),
                plan="Enterprise Annual Plan",
                windows=[
                    {"name": "5 hours", "used": 11,
                     "resetsAt": "2026-10-21T12:29:00+08:00"},
                    {"name": "7 days", "used": 22,
                     "resetsAt": "2026-10-25T12:29:00+08:00"},
                    {"name": "On-demand", "used": 33,
                     "resetsAt": "2026-10-26T12:29:00+08:00"},
                ],
            )
            for index in range(16)
        ]
        snapshot = mp.build_snapshot({"data": data}, now=NOW)
        for blob in snapshot.sub_bytes:
            self.assertLessEqual(len(blob), mp.SUB_PAYLOAD_MAX, blob)
        self.assertLessEqual(len(snapshot.index_bytes), mp.INDEX_PAYLOAD_MAX)

    def test_index_summary_is_the_worst_window(self):
        snapshot = mp.build_snapshot(load_quotas(), now=NOW)
        index = snapshot.index
        self.assertEqual(index["v"], 1)
        self.assertEqual(index["t"], NOW)
        self.assertEqual(index["lo"]["i"], 0)
        self.assertEqual(index["lo"]["u"], 100)
        self.assertEqual(index["lo"]["w"], "7d")
        self.assertEqual(index["lo"]["r"], 1792573226)
        self.assertEqual(index["lo"]["n"], snapshot.subs[0]["n"])

    def test_index_is_stable_without_time(self):
        first = mp.build_snapshot(load_quotas(), now=NOW)
        second = mp.build_snapshot(load_quotas(), now=NOW + 15)
        self.assertNotEqual(first.index_bytes, second.index_bytes)
        self.assertEqual(first.revision_key, second.revision_key)

    def test_revision_changes_with_data(self):
        quotas = load_quotas()
        first = mp.build_snapshot(quotas, now=NOW)
        quotas["data"][0]["windows"][0]["used"] = 99
        second = mp.build_snapshot(quotas, now=NOW)
        self.assertNotEqual(first.revision_key, second.revision_key)

    def test_derive_subscription_windows_helper(self):
        windows = mp.derive_subscription_windows(
            provider(windows=[
                {"name": "b", "used": 10},
                {"name": "a", "used": 30},
            ])
        )
        self.assertEqual([window["n"] for window in windows], ["a", "b"])


if __name__ == "__main__":
    unittest.main()
