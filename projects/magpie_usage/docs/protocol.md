# Magpie usage display — device protocol (v1)

The ESP-Mosaico usage display **pulls** its data. Two transports carry the same
bytes: a Bluetooth LE GATT profile served by the omarchy companion, and a
read-only HTTP endpoint as a fallback. Both are produced by
`companion/magpie_payload.py`, so a change of transport never changes the
payload.

Nothing here is a Magpie gateway key. Magpie trusts loopback callers, so the
companion reads `http://127.0.0.1:3425` without credentials; the device never
contacts Magpie and never stores a key.

## 1. Payloads

All keys are short and all values are integers, so a payload fits one ATT MTU
without long reads.

### INDEX

```json
{"v":1,"t":1792543800,"c":7,
 "lo":{"i":0,"n":"Kimi Code (China)","w":"7d","u":100,"r":1792573226},
 "ids":[0,1,2,3,4,5,6]}
```

| key | type | meaning |
|---|---|---|
| `v` | int | protocol version, currently `1` |
| `t` | int | sender Unix seconds when the snapshot was derived |
| `c` | int | subscription count, `0..16` |
| `lo` | object | the most-used quota window across all subscriptions |
| `lo.i` | int | slot index that owns that window |
| `lo.n` | string | that subscription's display name |
| `lo.w` | string | short window label (`5h`, `7d`, `od`, ...) |
| `lo.u` | int | used percent, `0..100` |
| `lo.r` | int | reset time as Unix seconds (`0` = unknown) |
| `ids` | int array | slot ids in page order; a client may fall back to `0..c-1` |

### SUB (one per page)

```json
{"i":2,"p":"claude","n":"Claude Code (2)","pl":"max","e":0,
 "w":[{"n":"5h","u":84,"r":1792549000},{"n":"7d","u":51,"r":1793000000}]}
```

| key | type | meaning |
|---|---|---|
| `i` | int | slot index; must match the slot that was requested |
| `p` | string | provider leaf id (`office/claude` → `claude`) |
| `n` | string | display name, ASCII, ≤ 28 bytes on screen |
| `pl` | string | plan, or `""` when Magpie has none |
| `e` | int | `1` when Magpie reported an error/sign-out for this provider |
| `w` | array | up to 3 windows, most-used first |
| `w[].n` | string | short window label |
| `w[].u` | int | used percent, `0..100` |
| `w[].r` | int | reset time as Unix seconds (`0` = unknown) |

`used` is what the device draws. Magpie reports both `used` and `remaining`;
the builder takes `used` when present and derives `100 - remaining` otherwise,
clamping to `0..100`.

### Size limits

| limit | bytes | why |
|---|---|---|
| `SUB_PAYLOAD_MAX` | 180 | fits a 185-byte ATT MTU (default is 247) |
| `INDEX_PAYLOAD_MAX` | 180 | `c ≤ 16` keeps `ids` around 48 bytes |

The builder shortens names, plans and window labels until the encoded payload
fits, and the tests assert the limit for every fixture subscription and for a
worst-case 16-subscription snapshot.

### Ordering and naming

1. Signed-out providers (`e=1`) sort last; providers with no usable window after
   them; then by most-used window descending; then by name.
2. At most 16 subscriptions are published.
3. Duplicate providers get a numeric suffix (`Claude Code (1)`,
   `Claude Code (2)`) so two accounts stay distinguishable.
4. The gateway's `· Remote magpie` suffix is stripped: it is noise on a 480 px
   page and costs bytes.

## 2. BLE profile

| item | value |
|---|---|
| Service | `7a3d0001-9c4e-4b8a-9f31-6d6167706965` |
| INDEX characteristic | `7a3d0002-9c4e-4b8a-9f31-6d6167706965`, `read` + `notify` |
| SUB characteristic | `7a3d0003-9c4e-4b8a-9f31-6d6167706965`, `write` + `read` |
| Local name | `magpie-usage` |
| Advertisement | flags + 128-bit service UUID + local name |
| ATT MTU | the central requests 247; payloads work from 185 up |

Read sequence for one refresh:

1. Subscribe to INDEX notifications (the central enables the CCCD).
2. Read INDEX — or use the value from the latest notification.
3. For each slot `0 .. c-1`: write the one-byte slot id to SUB, then read SUB.

INDEX notifies whenever the derived data changes. The sender timestamp alone
does not trigger a notification, so an idle display is not woken every 15 s.

### Encryption

Characteristics are plain `read`/`notify` by default. `magpie_ble_server.py
--encrypt` switches them to `encrypt-read` / `encrypt-notify` /
`encrypt-write`, which makes BlueZ bond before any value is exchanged.

Tradeoff: the payloads are derived usage percentages with no credentials, so
encryption protects little; BlueZ bonding here is Just Works (no MITM
protection) and adds a pairing step that must be repeated after the device
erases its bonds. It is therefore opt-in, for shared offices where even
aggregate usage should not be readable by a nearby scanner.

## 3. HTTP fallback

```
GET /v1/display/index          -> INDEX payload
GET /v1/display/sub/<slot>     -> SUB payload for <slot>
GET /v1/display/health         -> {"ok":true,"t":...,"c":...,"age_s":...,"error":...}
```

* The server only proxies derived data. There is no route that forwards a
  Magpie path, a Magpie key or an arbitrary URL.
* `Authorization: Bearer <token>` is optional. The token comes from
  `~/.config/magpie-display/env` (`MAGPIE_DISPLAY_TOKEN=...`) or
  `--token`/`MAGPIE_DISPLAY_TOKEN`, and is never committed.
* Responses are `Cache-Control: no-store`.
* Only GET is implemented; other methods answer `405`.

## 4. Device behaviour

* **Transport:** BLE first. After `CONFIG_MAGPIE_DISPLAY_BLE_FAILS_BEFORE_WIFI`
  failed connects the device prefers Wi-Fi and keeps retrying BLE with a 5 s …
  120 s backoff; a successful BLE pull switches it back.
* **Refresh:** every 15 s while connected (`CONFIG_MAGPIE_DISPLAY_REFRESH_INTERVAL_S`),
  plus an INDEX notification wake-up, plus a 30 s safety-net re-read.
* **STALE:** the header shows `STALE` after 60 s without a snapshot and
  `OFFLINE` after 180 s. The age comes from the device's own monotonic clock,
  so a sender with a wrong clock cannot freeze the display in `LIVE`.
* **Pages:** one subscription per swipe page; the footer shows page dots and
  `n/m`. Swiping past the last reported subscription snaps back to a live page.
* **Missing data:** a page whose SUB payload did not arrive shows `Loading...`;
  a signed-out provider shows `SIGNED OUT`; a provider with no windows shows
  `No quota windows`.
