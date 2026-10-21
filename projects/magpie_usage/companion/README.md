# Magpie usage companion (omarchy side)

These programs run on the machine that hosts the local Magpie gateway
(omarchy). They read **only** `127.0.0.1:3425`, derive a compact payload and
publish it to the ESP-Mosaico display over Bluetooth LE and/or HTTP. The device
pulls; nothing is pushed to the device and no port is opened on Magpie.

```
local Magpie :3425 (loopback, no credentials)
   /v1/magpie/quotas                  every 15 s
        |
        v
  magpie_payload.py  -> INDEX + one SUB payload per subscription (<= 180 B each)
        |
        +--> magpie_ble_server.py   BlueZ GATT peripheral (dbus-next)
        |       INDEX (read+notify) / SUB (write slot, then read)
        |
        +--> magpie_http_server.py  read-only HTTP (stdlib)
                GET /v1/display/index, /v1/display/sub/<i>
```

## Install

`dbus-next` is **not** preinstalled and is not a distributor package on
omarchy. Use a dedicated virtualenv (do not touch the system Python):

```sh
python3 -m venv ~/.venvs/magpie-display
~/.venvs/magpie-display/bin/pip install --upgrade pip dbus-next
```

`magpie_http_server.py` needs nothing outside the standard library, so the
HTTP fallback works even without the venv.

BlueZ 5.87 with the default D-Bus policy allows a user session to call
`org.bluez`; no extra policy file is required. The adapter must be powered:

```sh
bluetoothctl show            # "Powered: yes"
```

## Run

Both servers are independent; run either or both.

```sh
# Bluetooth LE peripheral (needs the venv)
~/.venvs/magpie-display/bin/python magpie_ble_server.py --adapter /org/bluez/hci0

# HTTP fallback (standard library only)
python3 magpie_http_server.py --bind 172.31.21.10 --port 8099
```

Useful flags:

| program | flag | meaning |
|---|---|---|
| both | `--magpie-url` | Magpie base URL, default `http://127.0.0.1:3425` |
| both | `--poll-interval` | seconds between Magpie reads, default 15 |
| `magpie_ble_server.py` | `--adapter` | BlueZ adapter object path |
| `magpie_ble_server.py` | `--encrypt` | bond before reads (see the tradeoff below) |
| `magpie_ble_server.py` | `--dry-run --once` | derive and print payloads, no D-Bus |
| `magpie_http_server.py` | `--bind` / `--port` | LAN address and port |
| `magpie_http_server.py` | `--env-file` | token file, default `~/.config/magpie-display/env` |

Dry run, useful before installing anything:

```sh
python3 magpie_ble_server.py --dry-run --once
```

## Token (optional)

The HTTP endpoint is read-only and exposes derived percentages, but on a shared
LAN a bearer token costs nothing:

```sh
install -d -m 700 ~/.config/magpie-display
printf 'MAGPIE_DISPLAY_TOKEN=%s\n' "$(python3 -c \
  'import secrets;print(secrets.token_urlsafe(24))')" \
  > ~/.config/magpie-display/env
chmod 600 ~/.config/magpie-display/env
```

Set the same value on the device (`CONFIG_MAGPIE_DISPLAY_WIFI_TOKEN` or the NVS
key `wifi_token` in namespace `magpie`). The file is never committed.

## systemd user units

Install and enable them:

```sh
install -d ~/.config/systemd/user
cp *.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now magpie-ble-server.service
systemctl --user enable --now magpie-http-server.service
systemctl --user status magpie-ble-server.service
journalctl --user -u magpie-ble-server -f
```

To keep the user units running without an active login session:

```sh
sudo loginctl enable-linger "$USER"
```

## Tests

```sh
python3 -m unittest discover -s tests
```

The payload builder and the HTTP endpoint need only the standard library; the
BLE tests import `dbus-next` and are skipped when it is missing. They never
touch a real adapter.

## Security notes

* Magpie itself is never bound to the LAN and no Magpie gateway key exists on
  disk. The device cannot reach Magpie directly — Magpie answers `401` to
  non-loopback callers without a caller key, and we deliberately do not create
  or ship one.
* The HTTP server serves only the two derived payloads; it has no proxy route.
* `--encrypt` is off by default: with Just Works bonding it adds a pairing step
  without MITM protection. See `../docs/protocol.md` §2.
