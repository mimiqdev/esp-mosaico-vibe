#!/usr/bin/env python3
"""BlueZ GATT peripheral for the Magpie usage display (dbus-next).

Profile (also documented in ``docs/protocol.md``):

    service  7a3d0001-9c4e-4b8a-9f31-6d6167706965
    INDEX    7a3d0002-...  read + notify   (INDEX payload)
    SUB      7a3d0003-...  write + read    (one SUB payload per selection)

The central writes a one-byte slot id to SUB and then reads it; INDEX notifies
whenever the derived data changes.  INDEX can also be read on connect, so a
central that misses the notification still gets a snapshot.

Read-only usage data contains no secrets.  ``encrypted=True`` switches the
reads/notifies to ``encrypt-read``/``encrypt-notify``, which makes BlueZ bond
(Just Works, no MITM protection) before any value is exchanged.  It is off by
default because the payloads are non-secret and pairing adds a failure mode on
every reinstall.
"""

from __future__ import annotations

import asyncio
from typing import Any, Awaitable, Callable

from dbus_next import Variant
from dbus_next.aio import MessageBus
from dbus_next.service import (
    PropertyAccess,
    ServiceInterface,
    dbus_property,
    method,
)

SERVICE_UUID = "7a3d0001-9c4e-4b8a-9f31-6d6167706965"
INDEX_UUID = "7a3d0002-9c4e-4b8a-9f31-6d6167706965"
SUB_UUID = "7a3d0003-9c4e-4b8a-9f31-6d6167706965"
LOCAL_NAME = "magpie-usage"

BLUEZ = "org.bluez"
ROOT_PATH = "/com/magpie/display"
SERVICE_PATH = ROOT_PATH + "/service0"
INDEX_PATH = SERVICE_PATH + "/index"
SUB_PATH = SERVICE_PATH + "/sub"
ADVERTISEMENT_PATH = ROOT_PATH + "/advertisement0"

DEFAULT_ADAPTER = "/org/bluez/hci0"


class BlueZError(RuntimeError):
    pass


class _ObjectManager(ServiceInterface):
    """BlueZ discovers the GATT hierarchy through ObjectManager."""

    def __init__(self, root_path: str) -> None:
        super().__init__("org.freedesktop.DBus.ObjectManager")
        self._root_path = root_path
        self._objects: dict[str, dict[str, dict[str, Variant]]] = {}

    def add_object(self, path: str, interface: str,
                   properties: dict[str, Variant]) -> None:
        self._objects.setdefault(path, {})[interface] = properties

    @method()
    def GetManagedObjects(self) -> "a{oa{sa{sv}}}":  # noqa: N802
        return self.managed_objects()

    def managed_objects(self) -> dict[str, dict[str, dict[str, Variant]]]:
        """Plain accessor: the @method() proxy cannot be called directly."""
        return self._objects


class _Characteristic(ServiceInterface):
    def __init__(
        self,
        uuid: str,
        flags: list[str],
        read: Callable[[], bytes],
        write: Callable[[bytes], None] | None = None,
    ) -> None:
        super().__init__("org.bluez.GattCharacteristic1")
        self._uuid = uuid
        self._flags = flags
        self._read = read
        self._write = write
        # BlueZ reads the Value property and forwards PropertiesChanged to
        # subscribed centrals; keep the last value here.
        self._value = b""
        self.notifying = False

    # -- properties -----------------------------------------------------
    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> "s":  # noqa: N802
        return self._uuid

    @dbus_property(access=PropertyAccess.READ)
    def Service(self) -> "o":  # noqa: N802
        return SERVICE_PATH

    @dbus_property(access=PropertyAccess.READ)
    def Flags(self) -> "as":  # noqa: N802
        return self._flags

    @dbus_property(access=PropertyAccess.READ)
    def Value(self) -> "ay":  # noqa: N802
        return self._value

    # -- methods --------------------------------------------------------
    @method()
    def ReadValue(self, options: "a{sv}") -> "ay":  # noqa: N802
        return self.do_read()

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):  # noqa: N802
        self.do_write(bytes(value))

    # Plain implementations: the @method() proxy cannot be called directly,
    # and the unit tests (plus any future non-D-Bus transport) use these.
    def do_read(self) -> bytes:
        self._value = self._read()
        return self._value

    def do_write(self, value: bytes) -> None:
        if self._write is None:
            raise BlueZError(f"{self._uuid} is read-only")
        self._write(value)

    @method()
    def StartNotify(self):  # noqa: N802
        self.notifying = True

    @method()
    def StopNotify(self):  # noqa: N802
        self.notifying = False

    def publish(self, value: bytes) -> None:
        """Update Value and emit PropertiesChanged (for StartNotify clients)."""
        self._value = value
        self.emit_properties_changed({"Value": value})


class _Service(ServiceInterface):
    def __init__(self, characteristic_paths: list[str]) -> None:
        super().__init__("org.bluez.GattService1")
        self._characteristics = characteristic_paths

    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> "s":  # noqa: N802
        return SERVICE_UUID

    @dbus_property(access=PropertyAccess.READ)
    def Primary(self) -> "b":  # noqa: N802
        return True

    @dbus_property(access=PropertyAccess.READ)
    def Characteristics(self) -> "ao":  # noqa: N802
        return self._characteristics


class _Advertisement(ServiceInterface):
    def __init__(self, encrypted: bool) -> None:
        super().__init__("org.bluez.LEAdvertisement1")
        self._encrypted = encrypted

    @dbus_property(access=PropertyAccess.READ)
    def Type(self) -> "s":  # noqa: N802
        return "peripheral"

    @dbus_property(access=PropertyAccess.READ)
    def ServiceUUIDs(self) -> "as":  # noqa: N802
        return [SERVICE_UUID]

    @dbus_property(access=PropertyAccess.READ)
    def LocalName(self) -> "s":  # noqa: N802
        return LOCAL_NAME

    @method()
    def Release(self):  # noqa: N802
        return None


async def _default_bus() -> MessageBus:
    bus = await MessageBus().connect()
    return bus


async def _manager(bus: Any, path: str, interface: str) -> Any:
    introspection = await bus.introspect(BLUEZ, path)
    obj = bus.get_proxy_object(BLUEZ, path, introspection)
    return obj.get_interface(interface)


class BlueZPeripheral:
    """Async lifecycle around the GATT application and advertisement."""

    def __init__(
        self,
        model: Any,
        adapter: str = DEFAULT_ADAPTER,
        encrypted: bool = False,
        bus_factory: Callable[[], Awaitable[Any]] | None = None,
    ) -> None:
        self.model = model
        self.adapter = adapter
        self.encrypted = encrypted
        self._bus_factory = bus_factory or _default_bus
        self.bus: Any | None = None
        self._object_manager: _ObjectManager | None = None
        self._index: _Characteristic | None = None
        self._sub: _Characteristic | None = None
        self._gatt_manager: Any | None = None
        self._adv_manager: Any | None = None
        self._registered = False

    def _build_objects(self) -> None:
        read_flags = ["read", "notify"]
        sub_flags = ["write", "read"]
        if self.encrypted:
            read_flags = ["encrypt-read", "encrypt-notify"]
            sub_flags = ["encrypt-write", "encrypt-read"]
        self._index = _Characteristic(
            INDEX_UUID, read_flags, read=lambda: self.model.read_index()
        )
        self._sub = _Characteristic(
            SUB_UUID,
            sub_flags,
            read=lambda: self.model.read_sub(),
            write=lambda value: self.model.write_selection(value),
        )
        self._service = _Service([INDEX_PATH, SUB_PATH])
        manager = _ObjectManager(ROOT_PATH)
        manager.add_object(
            SERVICE_PATH,
            "org.bluez.GattService1",
            {
                "UUID": Variant("s", SERVICE_UUID),
                "Primary": Variant("b", True),
                "Characteristics": Variant("ao", [INDEX_PATH, SUB_PATH]),
            },
        )
        manager.add_object(
            INDEX_PATH,
            "org.bluez.GattCharacteristic1",
            {
                "UUID": Variant("s", INDEX_UUID),
                "Service": Variant("o", SERVICE_PATH),
                "Flags": Variant("as", read_flags),
                "Value": Variant("ay", b""),
            },
        )
        manager.add_object(
            SUB_PATH,
            "org.bluez.GattCharacteristic1",
            {
                "UUID": Variant("s", SUB_UUID),
                "Service": Variant("o", SERVICE_PATH),
                "Flags": Variant("as", sub_flags),
                "Value": Variant("ay", b""),
            },
        )
        self._object_manager = manager

    async def start(self) -> None:
        if self.bus is not None:
            return
        self._build_objects()
        assert self._object_manager and self._index and self._sub
        bus = await self._bus_factory()
        self.bus = bus
        bus.export(ROOT_PATH, self._object_manager)
        bus.export(SERVICE_PATH, self._service)
        bus.export(INDEX_PATH, self._index)
        bus.export(SUB_PATH, self._sub)
        bus.export(ADVERTISEMENT_PATH, _Advertisement(self.encrypted))
        await bus.request_name("com.magpie.display")
        self._gatt_manager = await _manager(
            bus, self.adapter, "org.bluez.GattManager1"
        )
        await self._gatt_manager.call_register_application(ROOT_PATH, {})
        self._adv_manager = await _manager(
            bus, self.adapter, "org.bluez.LEAdvertisingManager1"
        )
        await self._adv_manager.call_register_advertisement(
            ADVERTISEMENT_PATH, {}
        )
        self._registered = True

    async def publish(self, index_bytes: bytes, changed: bool) -> None:
        """Publish the latest INDEX value; notify only when data changed."""
        if self._index is None:
            return
        if changed:
            self._index.publish(index_bytes)
        else:
            self._index._value = index_bytes

    async def stop(self) -> None:
        if self.bus is None:
            return
        try:
            if self._registered and self._adv_manager is not None:
                await self._adv_manager.call_unregister_advertisement(
                    ADVERTISEMENT_PATH
                )
            if self._registered and self._gatt_manager is not None:
                await self._gatt_manager.call_unregister_application(ROOT_PATH)
        except Exception as error:  # keep teardown best-effort
            print(f"magpie-ble: unregister failed: {error}")
        for path in (ADVERTISEMENT_PATH, SUB_PATH, INDEX_PATH, SERVICE_PATH,
                     ROOT_PATH):
            self.bus.unexport(path)
        self.bus.disconnect()
        self.bus = None
        self._registered = False


async def run(model: Any, adapter: str, encrypted: bool) -> None:
    """Minimal standalone runner used by ``magpie_ble_server --once`` dry runs."""
    peripheral = BlueZPeripheral(model, adapter=adapter, encrypted=encrypted)
    await peripheral.start()
    await peripheral.stop()
