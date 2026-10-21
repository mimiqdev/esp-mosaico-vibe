# ESP-Mosaico Magpie usage display

A 480×480 desk display that shows your LLM subscription usage, one subscription
per swipe page, pulled from a local Magpie gateway over Bluetooth LE with a
Wi-Fi HTTP fallback.

```
omarchy host (local Magpie on loopback)            ESP-Mosaico (ESP32-S31)
companion/magpie_payload.py  -- INDEX + SUB -->    main/magpie_source.h
  magpie_ble_server.py   (BlueZ GATT, pull)  -->     magpie_ble.c (NimBLE central)
  magpie_http_server.py  (read-only HTTP)    -->     magpie_wifi.c (esp_http_client)
                                                            |
                                                     magpie_store.c  (arbiter)
                                                     magpie_ui_state.c (strings)
                                                     magpie_ui.c     (GSP pages)
```

The device pulls; nothing is pushed to it and no port is opened on Magpie.
`docs/protocol.md` documents the payloads and both transports;
`companion/README.md` documents the sender installation.

## Layout

| path | contents |
|---|---|
| `main/magpie_parse.c`, `magpie_json.c` | portable payload decoder (no ESP-IDF) |
| `main/magpie_pull.c` | transport-agnostic refresh cycle |
| `main/magpie_ui_state.c` | portable display state and strings |
| `main/magpie_ui.c` | GSP binding (also compiled into the PC simulator) |
| `main/magpie_ble.c` | NimBLE central: scan, connect, discover, subscribe, read |
| `main/magpie_wifi.c` | HTTP fallback against the companion |
| `main/magpie_store.c` | snapshot store + BLE/Wi-Fi arbiter and backoff |
| `companion/` | payload builder, BLE peripheral, HTTP server, systemd units |
| `ui/generate.py` | generates `ui/main.json` (16 authored pages) |
| `tests/host/` | C host tests and payload fixtures |
| `pc/` | PC simulator backend and swipe validation script |

## Build and run

From the workspace root:

```sh
python mosaico.py project sim --project projects/magpie_usage --interactive
python mosaico.py recover
python mosaico.py iris system-update --project projects/magpie_usage
```

Run `recover` before the first install on a blank or unverified device.
Use `system-update` for new apps or changed resources/layouts; use `app-update`
only for code changes with an identical full partition table and resources.

## Tests

```sh
python3 tests/host/run_tests.py                       # C: decoder, pull, UI state
cd companion && python3 -m unittest discover -s tests  # builder, HTTP, BLE mocks
python3 pc/validate_sim.py --out /tmp/magpie-sim       # simulator swipe flow
```

The payload fixtures under `tests/host/fixtures/payloads/` are generated from
the captured Magpie document, so the decoder is tested against the exact bytes
the companion sends:

```sh
python3 tests/host/tools/make_payloads.py
python3 ui/generate.py          # after editing ui/generate.py
```

## Configuration

Kconfig (`menu "Magpie usage display"`) sets the transport preference, the BLE
name/scan/MTU, the companion HTTP host/port/token, the refresh cadence and the
STALE threshold. Runtime overrides live in NVS (namespace `magpie`) and are
documented in `main/magpie_config.h`. Wi-Fi credentials are read from the
`sysmeta` namespace written by `python mosaico.py iris test recovery-wifi`;
nothing secret is compiled in.
