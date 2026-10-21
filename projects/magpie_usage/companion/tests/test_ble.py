#!/usr/bin/env python3
"""Dry-run tests for the BLE peripheral.

The GATT model and the payload path are pure Python and always tested. The
``dbus-next`` interface classes are exercised with a mocked bus, so no BlueZ,
adapter or D-Bus daemon is needed on the test host.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPANION = HERE.parent
PROJECT = COMPANION.parent

sys.path.insert(0, str(COMPANION))

import magpie_ble_server as server  # noqa: E402
import magpie_payload as mp  # noqa: E402

QUOTAS = json.loads(
    (PROJECT / "tests" / "host" / "fixtures" / "quotas.json").read_text()
)
NOW = 1792543800

HAVE_DBUS_NEXT = importlib.util.find_spec("dbus_next") is not None


class GattModelTests(unittest.TestCase):
    def setUp(self):
        self.model = server.GattModel()
        self.snapshot = mp.build_snapshot(QUOTAS, now=NOW)

    def test_empty_model(self):
        self.assertEqual(json.loads(self.model.read_index())["c"], 0)
        self.assertEqual(self.model.read_sub(), b"{}")
        with self.assertRaises(ValueError):
            self.model.write_selection(b"\x00")

    def test_apply_and_read(self):
        self.assertTrue(self.model.apply(self.snapshot))
        index = json.loads(self.model.read_index())
        self.assertEqual(index["c"], self.snapshot.count)
        self.assertEqual(self.model.selected, 0)
        self.assertEqual(self.model.read_sub(), self.snapshot.sub_bytes[0])
        for slot, expected in enumerate(self.snapshot.sub_bytes):
            self.model.write_selection(bytes([slot]))
            self.assertEqual(self.model.read_sub(), expected)
            self.assertEqual(json.loads(expected)["i"], slot)

    def test_apply_reports_change(self):
        self.assertTrue(self.model.apply(self.snapshot))
        # The same data at a later sender time must not re-notify.
        later = mp.build_snapshot(QUOTAS, now=NOW + 15)
        self.model.apply(self.snapshot)
        self.assertFalse(self.model.apply(later))

    def test_write_rejects_bad_slots(self):
        self.model.apply(self.snapshot)
        with self.assertRaises(ValueError):
            self.model.write_selection(bytes([self.snapshot.count]))
        with self.assertRaises(ValueError):
            self.model.write_selection(b"\x00\x01")

    def test_selection_survives_shrinking_snapshot(self):
        self.model.apply(self.snapshot)
        self.model.write_selection(bytes([6]))
        smaller = mp.build_snapshot({"data": QUOTAS["data"][:2]}, now=NOW)
        self.model.apply(smaller)
        self.assertEqual(self.model.selected, 0)
        self.assertEqual(self.model.read_sub(), smaller.sub_bytes[0])

    def test_capacity_limit(self):
        data = [
            {
                "provider": f"p/{index}",
                "name": f"P{index}",
                "windows": [{"name": "7 days", "used": index}],
            }
            for index in range(20)
        ]
        snapshot = mp.build_snapshot({"data": data}, now=NOW)
        small = server.GattModel(max_subs=4)
        small.apply(snapshot)
        self.assertEqual(len(small.sub_bytes), 4)


@unittest.skipUnless(HAVE_DBUS_NEXT, "dbus-next is not installed")
class MockedDbusTests(unittest.TestCase):
    """Exercise the exported D-Bus objects through a fake message bus."""

    class FakeBus:
        def __init__(self):
            self.exported = {}
            self.requests = []
            self.name = None
            self.signal_calls = []

        def export(self, path, interface):
            self.exported[path] = interface

        def unexport(self, path):
            self.exported.pop(path, None)

        async def request_name(self, name):
            self.name = name

        async def introspect(self, destination, path):
            import dbus_next.introspection as intr

            if path.startswith("/org/bluez") or path == "/":
                return intr.Node([
                    intr.Interface("org.bluez.GattManager1"),
                    intr.Interface("org.bluez.LEAdvertisingManager1"),
                ])
            return intr.Node([])

        def get_proxy_object(self, destination, path, introspection):
            return self

        def get_interface(self, name):
            return self

        async def call_register_application(self, path, options):
            self.requests.append(("register_application", path))

        async def call_register_advertisement(self, path, options):
            self.requests.append(("register_advertisement", path))

        async def call_unregister_advertisement(self, path):
            self.requests.append(("unregister_advertisement", path))

        async def call_unregister_application(self, path):
            self.requests.append(("unregister_application", path))

        def disconnect(self):
            self.requests.append(("disconnect", None))

    def _peripheral(self, encrypted=False):
        from magpie_bluez import BlueZPeripheral

        model = server.GattModel()
        model.apply(mp.build_snapshot(QUOTAS, now=NOW))
        bus = self.FakeBus()

        async def factory():
            return bus

        return (
            model,
            bus,
            BlueZPeripheral(model, encrypted=encrypted, bus_factory=factory),
        )

    def test_start_and_stop_register_objects(self):
        model, bus, peripheral = self._peripheral()

        async def scenario():
            await peripheral.start()
            await peripheral.publish(model.index_bytes, True)
            await peripheral.publish(model.index_bytes, False)
            await peripheral.stop()

        asyncio.run(scenario())

        from magpie_bluez import (
            ADVERTISEMENT_PATH,
            INDEX_PATH,
            ROOT_PATH,
            SERVICE_UUID,
            SERVICE_PATH,
            SUB_PATH,
        )

        self.assertEqual(bus.name, "com.magpie.display")
        manager = bus.exported  # after stop everything is unexported
        self.assertEqual(manager, {})
        self.assertEqual(
            [request[0] for request in bus.requests],
            [
                "register_application",
                "register_advertisement",
                "unregister_advertisement",
                "unregister_application",
                "disconnect",
            ],
        )
        self.assertEqual(bus.requests[0][1], ROOT_PATH)
        self.assertEqual(bus.requests[1][1], ADVERTISEMENT_PATH)
        self.assertIn(SERVICE_PATH, (SERVICE_PATH, SUB_PATH, INDEX_PATH))
        self.assertTrue(SERVICE_UUID.endswith("6d6167706965"))

    def test_characteristic_values_and_flags(self):
        from magpie_bluez import INDEX_UUID, SUB_UUID, _Characteristic

        model, _bus, _peripheral = self._peripheral()
        index = _Characteristic(INDEX_UUID, ["read"],
                                read=lambda: model.read_index())
        sub = _Characteristic(
            SUB_UUID, ["write", "read"],
            read=lambda: model.read_sub(),
            write=lambda value: model.write_selection(value),
        )
        self.assertEqual(index.do_read(), model.index_bytes)
        sub.do_write(bytes([2]))
        self.assertEqual(json.loads(sub.do_read())["i"], 2)
        self.assertEqual(set(index.Flags), {"read"})
        self.assertEqual(set(sub.Flags), {"write", "read"})
        # Notify lifecycle for INDEX.
        index.StartNotify()
        self.assertTrue(index.notifying)
        index.publish(b"{}")
        self.assertEqual(index.Value, b"{}")
        index.StopNotify()
        self.assertFalse(index.notifying)

    def test_encrypted_flags(self):
        from magpie_bluez import BlueZPeripheral

        model = server.GattModel()
        peripheral = BlueZPeripheral(
            model, encrypted=True,
            bus_factory=lambda: self.FakeBus(),
        )
        peripheral._build_objects()
        self.assertIn("encrypt-read", peripheral._index.Flags)
        self.assertIn("encrypt-notify", peripheral._index.Flags)
        self.assertIn("encrypt-write", peripheral._sub.Flags)

    def test_object_manager_contents(self):
        from magpie_bluez import (
            INDEX_PATH,
            ROOT_PATH,
            SERVICE_PATH,
            SUB_PATH,
        )

        model, _bus, peripheral = self._peripheral()
        peripheral._build_objects()
        objects = peripheral._object_manager.managed_objects()
        self.assertEqual(set(objects), {SERVICE_PATH, INDEX_PATH, SUB_PATH})
        self.assertTrue(service_path_under(ROOT_PATH, SERVICE_PATH))
        characteristics = objects[SERVICE_PATH]["org.bluez.GattService1"][
            "Characteristics"
        ]
        self.assertEqual(characteristics.value, [INDEX_PATH, SUB_PATH])


def service_path_under(root: str, path: str) -> bool:
    return path.startswith(root)


if __name__ == "__main__":
    unittest.main()
