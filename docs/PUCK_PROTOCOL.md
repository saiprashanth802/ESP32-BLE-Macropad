# Encoder Puck — ESP-NOW Protocol

The puck is a standalone wireless knob: an **AS5600** magnetic encoder, a **128×64
SSD1306 OLED**, and a push button on an ESP32-C3/S3. It talks to the MacroPad v5 over
**ESP-NOW**; the pad translates puck input into BLE HID and sends it to whichever host
is currently active.

The pad is the only BLE endpoint. The puck never pairs with a computer.

```
  ┌──────────┐   ESP-NOW    ┌──────────────┐   BLE HID   ┌──────┐
  │  Puck    │ ───────────► │  MacroPad v5 │ ──────────► │ Host │
  │ AS5600   │ ◄─────────── │  (slot 1-3)  │             └──────┘
  │ + OLED   │   PK_STATE   └──────────────┘
  └──────────┘
```

## Wire format

One fixed 9-byte struct, used in both directions. **This table is the source of
truth** — the struct is duplicated in three sketches, because Arduino sketch folders
cannot share a header. All carry a `KEEP IN SYNC` marker:

| Sketch | Chip | Role |
|--------|------|------|
| `firmware/v5/macropad_v5/macropad_v5.ino` | ESP32 WROOM-32 | pad (receiver) |
| `firmware/v5/puck_encoder/puck_encoder.ino` | ESP32-C3 / S3 | puck |
| `firmware/v5/puck_encoder_8266/puck_encoder_8266.ino` | ESP-12E / ESP8266 | puck |

The two puck sketches share the protocol and tuning. Only the radio API and pin
numbers differ — plus OTA, which is C3-only. **ESP-NOW interoperates between ESP8266
and ESP32 over the air**, so either puck talks to the same pad firmware.

```c
struct PuckMsg {
  uint8_t ver;    // PUCK_PROTO_VER — receiver drops anything else
  uint8_t type;   // PK_*
  uint8_t mode;   // PM_VOLUME / PM_SCROLL / PM_ZOOM
  int8_t  ticks;  // signed detents in this packet, + = clockwise
  uint8_t btn;    // BTN_NONE / BTN_SHORT / BTN_DOUBLE / BTN_LONG
  uint8_t flags;  // pad → puck only (see below)
  uint8_t vol;    // pad → puck only: 0-100, or 0xFF unknown
  uint8_t speed;  // pad → puck only: detents per revolution, 8-128
  uint8_t accel;  // pad → puck only: acceleration cap, 1 = off, max 8
};
```

`PUCK_PROTO_VER` is currently **3**. Bump it on any field change; both sides reject
mismatched versions rather than misparsing. **v3 is not compatible with v2** — the
struct grew, and the length check drops short packets before the version check ever
runs. Flash both devices together, or the link goes silent with no error anywhere.

v2 added `vol` and moved mode ownership from the puck to the pad.
v3 added `speed`/`accel` and `PK_OTA`, moving dial feel to the pad as well.

| Type | Name | Direction | Meaning |
|------|------|-----------|---------|
| `0x01` | `PK_HELLO` | puck → pad | Sent on boot and every 5 s while idle. Registers the puck and keeps the link indicator alive. |
| `0x02` | `PK_INPUT` | puck → pad | Rotation and/or a button event. `ticks` and `btn` may both be set. |
| `0x03` | `PK_STATE` | pad → puck | Reply to any received packet. Carries `flags`, `vol`, `speed`, `accel`. |
| `0x04` | `PK_OTA` | pad → puck | Tear down ESP-NOW and raise the OTA access point. One-way — see below. |

### Sensitivity (`speed` / `accel`)

Both are owned by the pad, stored in its NVS (`pspd` / `pacc`), and pushed in every
`PK_STATE`. The puck holds no persistent copy: it boots on the compiled defaults and
adopts the pad's values on the first packet. One source of truth, same as mode.

`speed` is **detents per revolution**, not a raw threshold — that is the number a
human can reason about, and it keeps the AS5600's 4096-count resolution an
implementation detail of the puck. The puck derives `threshold = 4096 / speed`.

> **The sanity check that matters:** Windows moves volume **2% per
> `CONSUMER_VOL_UP`**, so **50 ticks is the entire 0→100% range**. That makes
> `speed = 50` "one full turn, one full sweep", and it is why the default is 50.
> Any tuning that puts far more than ~50 ticks in a comfortable revolution feels
> broken. Ticks per revolution is `speed × mult`, so `accel` multiplies this.

`accel` caps the velocity multiplier; `1` disables acceleration entirely. The
multiplier is `constrain(velocity × ACCEL_FACTOR, 1, accel)` with velocity in
counts/ms and `ACCEL_FACTOR = 1.0`, so it reads as "multiplier at 1 count/ms"
(≈90°/sec, a slow deliberate turn, = 1×).

⚠ There was once an extra `× 80.0f` in that expression. It pinned the multiplier at
the cap for **every** human-speed turn — reaching 1× needed a turn slower than
0.55°/sec — and combined with a 20-count threshold it swept the whole volume range
in about 11° of rotation. If the dial ever feels violently oversensitive again, that
expression is the first place to look.

### OTA (`PK_OTA`) — ESP32-C3 only

Sent from `Settings → PUCK → OTA` on the pad. On receipt the puck deinits ESP-NOW,
raises a SoftAP `Puck-Setup` / `puck12345`, and serves an upload page plus
`POST /api/update` at `192.168.4.1`.

**This is a one-way door.** ESP-NOW runs as an unassociated STA pinned to channel 1
and an AP will not come up cleanly underneath it, so the radio is torn down for good;
the only way back to normal operation is the reboot after flashing, or a power cycle.
The pad therefore refuses to send it unless the puck is currently linked, and sends
it three times, because a lost packet here is invisible — the user just sees no AP.

As with the pad's own OTA, **the access point disappearing is the success signal.**

The ESP-12 build has no OTA (no native USB, and the pad's trigger would strand it);
it logs a line to serial and carries on. Flash that board over USB.

### `flags` (pad → puck)

| Bit | Meaning |
|-----|---------|
| 0 | Pad's BLE link is up — input will actually reach a host |
| 1 | Pad was built with mouse HID, so `PM_SCROLL` is supported |
| 2 | Pad has the puck **disabled** in Settings (input is being discarded) |
| 3 | `vol` is a real level (the companion app is feeding it) |
| 4 | System is muted |

The puck renders these on the OLED, so a dead knob explains itself instead of just
doing nothing.

## Modes

The knob is modal; a long button press cycles the mode. Which modes actually work
depends on the pad's HID descriptor:

| Mode | Rotation sends | HID used | Works today? |
|------|----------------|----------|--------------|
| `PM_VOLUME` (0) | Volume up/down | Consumer (Report ID 2) | **Yes** |
| `PM_ZOOM` (2) | `Ctrl` + `+` / `Ctrl` + `-` | Keyboard (Report ID 1) | **Yes** |
| `PM_SCROLL` (1) | Mouse wheel | Mouse (Report ID 3) | **Only if `PUCK_MOUSE_HID 1`** |

Volume and zoom need **no change to the pad's HID descriptor** — they reuse the
keyboard and consumer reports the pad already advertises.

Scroll needs a third HID report. Adding it changes the GATT database, and on Windows
that reliably wedges service discovery with `0x8000FFFF` until you toggle Bluetooth
off and on (see `HARDWARE_HANDOFF.md`). So it is compiled out by default:

```c
#define PUCK_MOUSE_HID 0   // 1 = add Report ID 3 and enable PM_SCROLL
```

With the flag at 0, the pad reports scroll as unsupported via `flags` bit 1 and the
puck skips `PM_SCROLL` when cycling modes, so the knob never lands in a dead mode.

## Mode ownership — the pad decides

**The pad owns the dial mode.** The puck renders whatever arrives in `PK_STATE` and
never changes mode on its own; `m->mode` on a puck→pad packet is deliberately
ignored. This exists so a pad key and the puck's (optional) button can both drive
mode without two copies of the state drifting apart.

- Pad key: bind the **`PuckMode`** action (`A_PUCK_MODE`, id 132) to any key. It
  cycles the mode, toasts the new one, and pushes it down immediately. It works
  with no BLE host connected, since it is a pad-local setting rather than a
  keystroke.
- Puck button: a long press sends `BTN_LONG`, which is a *request*. The pad cycles
  and the new mode comes back in the next `PK_STATE`.

`PM_SCROLL` is skipped while `PUCK_MOUSE_HID` is 0, so cycling can never land on a
mode that silently does nothing.

## Volume readout

`vol` carries the **true system volume**, which neither device can otherwise know:
BLE HID volume is relative, so the pad sends "up"/"down" usages and is never told
the resulting level.

The path is `Windows Core Audio → companion app → HCMD_VOLUME (0x8B) → pad →
PK_STATE → puck`. The companion polls every 150 ms and writes only on change, so a
steady volume produces no BLE traffic. The pad relays immediately on receipt, so the
readout tracks the Windows slider even when the change came from somewhere else.

Without the companion running, `PF_VOL_KNOWN` stays clear and the puck simply shows
the mode name instead of a bar. The puck deliberately does **not** estimate a level
from its own tick count — that desyncs the first time volume changes from anywhere
else, and a confidently wrong number is worse than none.

The puck shows the bar only while the dial is in use (`BAR_HOLD_MS`, 1.5 s after the
last detent) and then reverts to the mode name.

## Button

| Gesture | Action |
|---------|--------|
| Short click | Play / pause |
| Double click | Next track |
| Long press (≥ 600 ms) | Cycle mode |

Button actions are consumer-control usages and are mode-independent.

## Pairing

The puck hardcodes the pad's **station MAC**. On the v5 bench board that is the base
MAC as printed by `WiFi.macAddress()`:

```
84:1F:E8:2B:33:48
```

Note this is *not* the BLE address the companion app connects to — that one is base
MAC **+ 2** (`84:1F:E8:2B:33:4A`). The two radios have different addresses on the same
chip; using the BLE address here is a silent failure mode.

The pad does **not** hardcode the puck's MAC. It learns the address from
`esp_now_recv_info_t->src_addr` on the first packet and adds it as a peer so it can
reply. Reflashing or replacing the puck therefore needs no pad-side change.

## ESP8266 vs ESP32 — API differences

Both chips speak the same ESP-NOW over the air, but the Arduino APIs are not
source-compatible. If you port changes between the two puck sketches, these are the
four places they diverge:

| | ESP32-C3 | ESP8266 |
|---|---|---|
| Header | `esp_now.h` | `espnow.h` |
| Init success | `== ESP_OK` | `== 0` |
| Role | n/a | `esp_now_set_self_role(ESP_NOW_ROLE_COMBO)` — **required**, and the link silently fails without it |
| Recv callback | `(const esp_now_recv_info_t*, const uint8_t*, int)` | `(uint8_t* mac, uint8_t* data, uint8_t len)` |
| Add peer | `esp_now_add_peer(&peer_info_struct)` | `esp_now_add_peer(mac, role, channel, key, keylen)` |
| Set channel | `esp_wifi_set_channel()` | `wifi_set_channel()` (from `user_interface.h`) |

`COMBO` is the role to use on the 8266: the puck both sends input and receives
`PK_STATE`, and `CONTROLLER`/`SLAVE` are each one-directional.

Pins differ too — the ESP-12 uses `SDA=GPIO4`, `SCL=GPIO5`, `BTN=GPIO13`. GPIO 0, 2
and 15 are strapping pins and are deliberately unused; a peripheral holding one at
the wrong level stops the module booting at all.

## Channel

Both ends pin themselves to **channel 1** (`PUCK_WIFI_CHANNEL`). ESP-NOW peers must
share a channel, and an unconnected station defaults to 1 on both chips — pinning it
explicitly just removes the ambiguity.

## Radio coexistence

This is the one genuine risk in the design. The pad runs **NimBLE and WiFi at the same
time** to receive ESP-NOW, sharing a single antenna. `docs/ROADMAP.md` records this
causing connection instability at high packet rates on v4.

Mitigations, all in the puck:

- Rotation is coalesced and sent at most once per `SEND_INTERVAL_MS` (40 ms), so a
  fast spin is a handful of packets, not hundreds.
- `PK_HELLO` heartbeats stop while the knob is active.
- The pad-side receiver is off by default (`Settings → PUCK`) and stored in NVS, so a
  pad with no puck never brings up the WiFi stack at all.

If BLE gets flaky, raise `SEND_INTERVAL_MS` before suspecting anything else.

## Sleep — a real limitation

**The puck cannot wake a sleeping pad.** The pad's idle timeout calls
`esp_light_sleep_start()`, which powers the WiFi radio down; ESP-NOW packets sent
during that window are simply lost. Waking is GPIO-only, from the key matrix.

So: press any pad key first, then use the knob. Or set `Settings → SLEEP` to `OFF`
if the puck is your main input.

This is not fixable without giving up light sleep. ESP-NOW has no low-power receive
mode in station-idle — staying reachable means keeping the radio on, which is most of
the idle current the sleep timeout exists to save.

On wake, the pad tears ESP-NOW down and re-initialises it rather than trusting state
that survived a radio power cycle. The puck is re-learned from its next packet, which
is at most one heartbeat (5 s) away, or immediate if you touch the knob.

## Config Mode interaction

The pad's Config Mode (`Settings → CONFIG`) tears down BLE and raises a SoftAP for
OTA. ESP-NOW is stopped and the station interface dropped before the AP comes up —
the two cannot share the interface, and leaving ESP-NOW running makes `softAP()` fail
to come up cleanly. Leaving Config Mode reboots the pad, which restores ESP-NOW from
NVS on the next boot.
