# Dial Satellite — Design Spec & Link Protocol

Wireless rotary-dial satellite for the MacroPad, linked over ESP-NOW.

- **Status:** design frozen for v1; firmware not yet written.
- **Date:** 2026-07-27
- **Scope of v1:** bare ESP-12E (ESP8266) + AS5600 magnetic encoder, one-way
  link to the pad, volume by default, mode changed from a pad key.
- **Deliberately deferred:** OLED, on-satellite buttons, potentiometer slider,
  battery power.
- **Dropped, not deferred:** microphone — not achievable on the ESP8266. It
  returns only if the satellite moves to the ESP32-S3 (§9).

This document is the stable contract. The slider and mic are expected later and
the packet format is versioned and type-tagged so they can be added without
changing the pad's receive path.

---

## 1. Why a satellite at all

The pad is a 4×3 matrix with no rotary input. A dial is the natural control for
continuous quantities (volume, zoom) that keys handle badly. Putting it on a
separate board rather than on the pad is a deliberate repeat of the decision
already made for the AS5600 puck: different duty cycle, independent failure
domain, and it can sit somewhere on the desk that the pad does not.

## 2. Architecture

```
┌────────────────────────────┐          ┌──────────────────────────────┐
│  Satellite (ESP-12E/8266)  │          │  MacroPad (ESP32 WROOM-32)   │
│                            │  ESP-NOW │                              │
│  AS5600 ──I2C──> tick      │ ───────> │  RX callback → mailbox       │
│           accumulator      │  ch 1    │            ↓                 │
│           ≤40 Hz send slot │  bcast   │  loop(): dialTick()          │
│                            │          │            ↓                 │
│  (no display, no buttons)  │          │  handleDialTicks(delta)      │
└────────────────────────────┘          │            ↓                 │
                                        │  sendConsumer() / sendKey()  │
                                        │            ↓ BLE HID         │
                                        └──────────────────────────────┘
                                                     ↓
                                                    Host
```

The satellite is a dumb sensor. It holds no mode, no configuration, and nothing
worth persisting. **All interpretation lives on the pad.** This is what allows
the dial's behaviour to be changed from a pad key without any back-channel, and
it is why v1 needs no pad→satellite path at all.

### Why one-way

A pad→satellite channel would only be needed to drive a display on the
satellite. v1 has no display, so the link is strictly one-way. Adding the
reverse direction later is additive: it does not invalidate anything here.

## 3. Hardware

### 3.1 Satellite — bare ESP-12E (ESP8266)

**ESP-12E module** + AS5600 (Robu 23×23 mm breakout). USB-powered for the
prototype; battery is out of scope for v1 and must not gate the link working.

The ESP-12E was chosen over the ESP32-S3 SuperMini on cost, and it is the
module the original `CONTEXT.md` puck plan already specified. The trade is
explicit and accepted: **the ESP8266 has one ADC (A0, 0–1 V) and no practical
microphone path.** A slider remains possible on A0 with a divider; a mic does
not. If the mic becomes a real requirement the satellite moves to the S3 — see
§9, where that swap costs one sketch and nothing else.

**Bare module, not a dev kit.** There is no USB, no regulator and no
auto-reset circuit on board; a programming jig has to be built. The module is
also **2 mm pitch**, so it needs an ESP-12 adapter PCB or hand-soldered wires —
it will not sit in a breadboard. Jig wiring, flash procedure and IDE settings
are in `firmware/tests/esp12e_bringup.ino`, which is the first thing flashed.

**Boot-pin biasing (from `CONTEXT.md` §2, unchanged):** EN, RST, GPIO0 and
GPIO2 pulled **up** through 10 kΩ; GPIO15 pulled **down** through 10 kΩ.
**Confirmed unusable:** GPIO9, GPIO10, and the flash-bus SCLK pin.

**Pin map (proposed, pending bench confirmation):**

| AS5600 pin | ESP-12E | Notes |
|---|---|---|
| VCC | 3V3 | **Not 5V.** AS5600 is a 3.3 V part. |
| GND | GND | |
| SDA | **GPIO 4** | `Wire.begin(4, 5)` — software I2C on the ESP8266. |
| SCL | **GPIO 5** | |
| DIR | GND | Fixes rotation polarity. Floating ⇒ direction undefined. |
| OUT | — | Unconnected; the analog/PWM output is unused. |

GPIO 4 and 5 are the conventional ESP8266 I2C pair, have no boot-strapping
role, and are the only two free pins that are unambiguously safe. This is a
deliberate deviation from `CONTEXT.md`, which put the AS5600 on GPIO 12/13 and
reserved 4/5 for the puck's two buttons — v1 has no buttons, so the safer pins
are now free and are used instead. GPIO 12/13/14 remain available if bench
testing gives a reason to move.

I2C on the ESP8266 is bit-banged in software rather than a hardware peripheral.
It is entirely adequate for polling one AS5600, but it is not free: the poll
loop in §4.4 competes with the WiFi stack for CPU, which is a reason to keep
the poll rate modest and let §7.1 confirm it rather than assume.

If the breakout has no onboard I2C pull-ups, add 4.7 kΩ (or the 10 kΩ parts
already on hand) from SDA and SCL to 3V3.

**Power.** Do not run the module from a USB-TTL adapter's 3.3 V pin — those
supply 50–100 mA against an ESP8266 that spikes to 300–500 mA on radio TX. Use
an AMS1117-3.3 off the adapter's 5 V rail, with 10 µF + 100 nF decoupling close
to the module. Brownout presents as random resets and corrupt serial, which is
easily mistaken for a firmware bug; `esp12e_bringup.ino` prints the reset reason
specifically so this gets diagnosed correctly.

**Magnet:** diametrically magnetised, 0.5–3 mm above the AS5600 die, centred on
it. The AS5600 `STATUS` register (0x0B) reports MD/ML/MH — magnet detected, too
weak, too strong. Bench validation must read it rather than assume placement.

Per the project's hardware-first workflow this pin map is **proposed, not
locked**. It is confirmed by the bench step in §7.1 before any link firmware is
written.

### 3.2 Pad

No hardware change. The existing ESP32 WROOM-32 gains a WiFi/ESP-NOW radio path
alongside NimBLE — see §6 for why that is the main risk and how it is bounded.

## 4. Link protocol

### 4.1 Radio configuration

Both ends run `WiFi.mode(WIFI_STA)` with no AP association, on a channel fixed
to **1**. ESP-NOW peers must agree on channel; pinning it removes a whole class
of intermittent-link bugs.

**The two ends use different APIs — this link is cross-family.** The satellite
is an ESP8266 and the pad is an ESP32, so the ESP-NOW setup is not symmetric:

| | Satellite (ESP8266 / ESP-12E) | Pad (ESP32 WROOM-32) |
|---|---|---|
| Header | `espnow.h` (ESP8266 core) | `esp_now.h` (ESP-IDF) |
| Channel | `wifi_set_channel(1)` | `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)` |
| Role | `esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER)` | no role call needed |
| Peer | `esp_now_add_peer(mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0)` | `esp_now_peer_info_t` |
| Callback | `esp_now_register_recv_cb` — not used, TX only | `esp_now_register_recv_cb` |

Cross-family ESP-NOW is supported and works, but it is fussier than
ESP32↔ESP32, and the ESP8266's explicit **role** concept is the usual thing
that gets missed — a controller that never had its self-role set transmits
nothing, silently. Channel disagreement is the second. §7.1 step 3 exists to
prove this link before anything is built on it.

The 12-byte `SatPacket` in §4.3 is well within the ESP-NOW payload limit on
both families, so the packet format itself carries no interop risk.

### 4.2 Addressing — broadcast, not paired MAC

The satellite transmits to the broadcast address `FF:FF:FF:FF:FF:FF`. The pad
accepts any packet whose `magic` and `ver` fields match and ignores everything
else.

This is chosen over hardcoding the pad's MAC because there is no display and no
button on the satellite, so there is no way to run a pairing UI on it, and
hardcoding would require reflashing the satellite whenever the pad changes.
The cost is that the link is unauthenticated and unencrypted — ESP-NOW cannot
encrypt broadcast traffic. For a personal desk peripheral carrying volume ticks
this is an acceptable trade; it is recorded here so the decision is not
mistaken for an oversight. If it ever matters, the upgrade path is a shared
4-byte link ID stored in NVS on both ends, not a change to the packet layout.

### 4.3 Packet format

Fixed 12 bytes, little-endian, ESP-NOW payload. Version 1.

```c
// Sent satellite → pad. Never sent in the other direction in v1.
typedef struct __attribute__((packed)) {
  uint32_t magic;    // 0x3153504D  ('MPS1' LE) — link filter
  uint8_t  ver;      // protocol version; 1
  uint8_t  type;     // SAT_DIAL = 0x01
  uint8_t  seq;      // rolling 0..255, increments per packet
  uint8_t  flags;    // bit0 reserved (low battery), bit1 = first packet since boot
  int16_t  delta;    // signed detent ticks accumulated since last packet
  uint16_t value;    // 0 for SAT_DIAL; absolute value for future sensors
} SatPacket;         // sizeof == 12
```

**Type codes.** `0x01` dial. Reserved for later: `0x02` slider (uses `value` as
absolute 0–1000, `delta` unused), `0x03` mic control events. Codes the pad does
not recognise are dropped silently — that is what makes adding the slider a
satellite-only change.

**`seq`** lets the pad drop duplicates. ESP-NOW does not retransmit at this
layer, but a repeated `seq` with identical contents indicates a duplicate and is
discarded; a gap simply means a lost packet and is not corrected. Losing a
packet costs a few ticks of dial movement, which is not worth retransmitting.

**`flags` bit1** marks the first packet after the satellite boots, so the pad can
reset link state and show a toast rather than treating a large first `delta` as
real movement.

### 4.4 Transmit policy — the rate cap

This is the single most important behavioural rule in the design, for the reason
in §6.

- The satellite polls the AS5600 at ~200 Hz and accumulates detent ticks
  locally. Polling is local I2C traffic and costs the radio nothing.
- It transmits **at most once every 25 ms (≤40 packets/s)**, and only when the
  accumulated tick count is non-zero. The accumulator is zeroed on send.
- **Spinning the dial faster increases `delta`, never the packet rate.** This is
  the property that keeps the link away from the failure mode recorded in
  `ROADMAP.md:79`.
- An idle dial transmits **nothing at all** — no keepalive, no heartbeat.

### 4.5 Tick derivation

The AS5600 reports a 12-bit absolute angle (0–4095) from register 0x0E/0x0F. The
satellite tracks the previous raw angle, computes the shortest-path signed
difference (handling the 4095→0 wrap), accumulates it, and emits one detent tick
per `TICK_THRESHOLD` raw counts. Threshold and any acceleration curve are
satellite-side tuning constants, established on the bench in §7.1 — the pad must
work correctly for any `delta` it receives and must not encode assumptions about
the dial's physical resolution.

## 5. Pad-side integration

### 5.1 Dial modes

```
DIAL_VOL     (default)  ticks → consumer VOLUME_UP / VOLUME_DOWN
DIAL_ZOOM               ticks → Ctrl +  /  Ctrl −      (OS-layout aware)
DIAL_ALTTAB             ticks → Alt+Tab / Alt+Shift+Tab
```

**SCROLL is deliberately absent from v1.** The pad's HID report map is keyboard
(Report ID 1) + consumer control (Report ID 2) only — there is no mouse report,
so there is no wheel to drive. Adding one changes the HID report descriptor,
which risks invalidating existing bonds across all three host slots. That is a
larger change than this feature justifies and is tracked separately.

`dialMode` is a single global, persisted to NVS through the existing
`saveSettings()` path. It is not per-preset: the user asked for volume by
default with the mode changed explicitly, and a global mode means the dial does
not silently change meaning when a preset switches.

### 5.2 Changing the mode

A new entry in `ACTION_LIB`:

```c
{"Dial Mod", A_DIAL_MODE},
```

Tapping a key bound to it cycles VOL → ZOOM → ALT-TAB → VOL and shows a toast
naming the new mode.

This reuses the existing per-preset key assignment, BUILD MODE, and companion
`HCMD_KEY` machinery wholesale. **No new settings screen, no new UI state, no
new persistence path.** The user binds it to whichever key in whichever presets
they want.

### 5.3 Receive path and task safety

The ESP-NOW receive callback runs in the WiFi task. The project's existing rule
— established for `pendingBleEvent` and the `hostCmdQ` mailbox — is that only
the loop task touches the TFT. The dial path follows it exactly:

1. `onEspNowRecv()` validates `magic`/`ver`/`type`/`seq`, adds `delta` into a
   `volatile int16_t` accumulator, stamps `lastDialPacketMs`, and returns. It
   draws nothing and calls no BLE function.
2. `dialTick()`, called from `loop()`, drains the accumulator and calls
   `handleDialTicks(delta)`.
3. `handleDialTicks()` emits HID via the existing `sendConsumer()` / `sendKey()`
   and calls `recordActivity()` so the dial defers the sleep timer.

Output is rate-limited on the pad as well: a large `delta` is emitted as a
bounded burst rather than an unbounded loop, so one lost second of packets
cannot stall the loop task or flood the host.

### 5.4 Link state

`lastDialPacketMs` drives a small indicator in the existing status bar. No
packet for 5 s ⇒ shown as idle. This is presence, not health: an idle dial is
indistinguishable from an absent one by design, since §4.4 forbids keepalives.

### 5.5 Settings toggle

Settings gains **Dial link ON/OFF**, persisted, default ON. Turning it off calls
`esp_now_deinit()` and `WiFi.mode(WIFI_OFF)`, fully releasing the radio and its
RAM.

Note the deliberate difference from v4, which defaulted ESP-NOW **off**
(`TESTING_LOG.md:40`). Here the rate cap in §4.4 is the primary mitigation, so
the link is expected to be safe to leave on; the toggle exists as a one-tap
recovery for a user hitting instability, not as the mitigation itself. If §7.1
step 4 shows BLE disconnects with the cap in place, the default flips to OFF and
the rate cap has failed as a strategy.

## 6. The coexistence risk

The pad's ESP32 has one 2.4 GHz radio shared by BLE and WiFi. The project has
already been here and the evidence is on record:

- `docs/TESTING_LOG.md:39` — "BLE connection instability when WiFi (ESP-NOW)
  active simultaneously." Mitigated in v4 by defaulting ESP-NOW off.
- `docs/ROADMAP.md:79` — "Coexist but cause connection instability **at high
  packet rates**."
- `docs/CONFIG_API.md:11` — Config Mode tears BLE down entirely before starting
  WiFi, because "WiFi and BLE don't coexist well on the WROOM-32."

Two things follow.

**The failure mode is rate, not coexistence per se.** v4's sender transmitted one
packet per tick, so a fast spin produced a burst at whatever rate the encoder
could generate. §4.4 caps the rate at 40 Hz independent of dial speed and sends
nothing when idle. That is the primary mitigation and the reason the transmit
policy is specified as a requirement rather than left to the implementation.

**Heap is the second constraint.** The WiFi stack costs roughly 40–50 KB on top
of NimBLE, and `macropad_v5.ino:845` already notes that a full 320×240 sprite is
"impossible with BLE active". Whether the remaining headroom absorbs WiFi is
unknown and cannot be settled by reading code.

**Therefore §7.1 step 2 is a hard gate.** No feature code is written until a
minimal spike shows the pad can hold a BLE HID connection with ESP-NOW
initialised, with the free-heap numbers recorded. If it fails, the fallback is
decided before any further work. The fallback is routing the satellite to the
**companion app** instead of the pad: the satellite joins WiFi and talks to the
Fedora companion, which drives volume over the existing host-link. This costs
the "works on any paired host" property and only functions where the companion
runs, which is why it is the fallback and not the design.

(An earlier draft named a second ESP8266 as a pad-side ESP-NOW→UART
co-processor. That option is now spent — the ESP-12E on hand is the satellite,
per §3.1.)

## 7. Validation plan

Every step below produces a dated entry in `docs/DIAL_SATELLITE_BUILDLOG.md`
with the actual serial output, not a summary.

### 7.1 Bench sequence

0. **ESP-12E bring-up.** Flash `firmware/tests/esp12e_bringup.ino` over the jig
   in §3.1. Pass = steady 1 Hz heartbeat, and a reset reason of "Power on" or
   "External System" — never WDT or Exception. **This proves the jig and the
   supply before any peripheral is attached**, so later faults can be blamed on
   the right thing. Record the printed STA MAC.
1. **AS5600 read on the ESP-12E.** Adapt `firmware/tests/AS5600_test.ino` to the
   ESP8266 and the §3.1 pin map. Pass = stable angle across a full rotation,
   `STATUS` reporting MD set with ML and MH clear. **This locks the pin map.**
2. **Coexistence gate.** Add `WiFi.mode(WIFI_STA)` + `esp_now_init()` to the
   live v5 firmware and nothing else. Record free heap before and after init.
   Pass = BLE HID still types reliably and heap headroom is sufficient. **No
   further work if this fails** — see §6.
3. **Link up.** Satellite transmits, pad logs `delta` to serial. Pass = ticks
   arrive with correct sign and no duplicates.
4. **Rate ceiling.** Spin the dial as fast as possible for 30 s. Pass =
   measured packet rate stays at or below 40 Hz and BLE stays connected.
5. **Volume end to end.** Pass = host volume tracks the dial with no runaway and
   no stuck keys.
6. **Mode cycling.** Bind `A_DIAL_MODE` to a key; verify all three modes and
   that the mode survives a reboot.
7. **Sleep interaction.** Confirm the documented behaviour in §8 — dial input
   while the pad sleeps is lost, and the pad wakes normally on a key press with
   the dial working immediately after.

### 7.2 Recorded measurements

The build log must carry actual numbers for: free heap before/after ESP-NOW
init, measured peak packet rate, dial-to-host latency, and BLE disconnect count
over a 30-minute soak with the dial in active use.

## 8. Known limitations of v1

These are accepted, not defects.

- **Dial input while the pad is asleep is lost.** Light sleep powers down the
  radio, so a tick cannot wake the pad. Press any key to wake; the dial works
  immediately after. Keeping WiFi alive through sleep would defeat the pad's
  power design.
- **No SCROLL mode** — see §5.1.
- **Link is unauthenticated** — see §4.2.
- **No battery telemetry.** `flags` bit0 is reserved for it; the satellite is
  USB-powered in v1.
- **Lost packets are not retransmitted** — see §4.3.

## 9. Extension path

Nothing here needs revisiting to add the deferred hardware:

- **Slider:** satellite sends `type = 0x02` with `value` = absolute 0–1000. The
  pad adds a handler; the packet layout, addressing and rate cap are unchanged.
  Absolute position has no HID equivalent, so a per-preset assignable slider
  will likely drive the companion app over the existing host-link GATT — a new
  `HEV_` opcode in `CONFIG_API.md`, not a change to this protocol.
- **OLED on the satellite:** requires a pad→satellite path, which is additive.
  The `SatPacket` layout is reusable in reverse with new type codes.
- **Microphone:** not achievable on the ESP8266 and therefore out of scope while
  the ESP-12E is the satellite. Control events would fit `type = 0x03`, but
  **audio streaming does not fit this protocol at all** — 12-byte control
  packets at 40 Hz and an audio stream are different problems. Do not attempt to
  extend this format for it.

- **Swapping the satellite to the ESP32-S3 SuperMini:** the intended path if the
  mic or a second analog input becomes real. Cost is **one sketch**: the pad,
  the packet format, the addressing and the rate cap are all unchanged, because
  §2 puts every decision on the pad and leaves the satellite a dumb sensor. The
  S3 changes are the pin map (`SDA GPIO 8 / SCL GPIO 7` — GPIO 9, the usual S3
  default for SCL, is outside the SuperMini's twelve boot/flash-free pins
  `IO1, IO2, IO4, IO5, IO6, IO7, IO8, IO15, IO16, IO17, IO18, IO21`), hardware
  rather than bit-banged I2C, and the ESP32 ESP-NOW API — which makes the link
  same-family and removes the §4.1 interop risk entirely.

## 10. Deliverables

| Document | Purpose |
|---|---|
| `docs/DIAL_SATELLITE.md` | This spec — frozen decisions and the link contract. |
| `docs/DIAL_SATELLITE_BUILDLOG.md` | Dated engineering log: every bench test, measurement, failure and fix, with real serial output. |
| `docs/DIAL_SATELLITE_BUILD.md` | User-facing build guide: BOM, wiring, flashing, first-run, troubleshooting. |
| §7 results in the build log | Bench validation record: pass/fail per subsystem with numbers. |

## 11. Decision log

| Decision | Rationale |
|---|---|
| Satellite is a dumb sensor; pad owns all interpretation | Lets a pad key change dial behaviour with no back-channel. |
| One-way link in v1 | Nothing on the satellite needs to display state. |
| Broadcast addressing | No display or button on the satellite, so no pairing UI is possible. |
| Rate cap as a spec requirement | v4's per-tick sending is the recorded cause of BLE instability. |
| Global `dialMode`, not per-preset | The dial should not silently change meaning on a preset switch. |
| `A_DIAL_MODE` as an `ACTION_LIB` entry | Reuses all existing assignment/persistence machinery; no new UI. |
| SCROLL deferred | Needs a mouse HID report, which risks existing bonds. |
| ESP-12E as the satellite, not the ESP32-S3 | Cost, and it is the module `CONTEXT.md` already planned for. Accepts losing the mic; §9 keeps the S3 swap to one sketch. |
| AS5600 on GPIO 4/5, not `CONTEXT.md`'s 12/13 | v1 has no buttons, so the conventional boot-free I2C pair is free and is the safer choice. |
| Coexistence spike as a hard gate | Prior bench evidence says this may not work; find out before building on it. |
| Bring-up sketch before any peripheral | A bare module with a hand-built jig fails at the supply far more often than at the code; prove power first so later faults are attributed correctly. |

## 12. History

- 2026-07-27 — spec written. Scope set to dial-only after OLED, slider,
  satellite buttons and mic were deferred out of v1. Rate cap promoted to a
  requirement on the strength of the v4 findings in `TESTING_LOG.md` and
  `ROADMAP.md`. Firmware not yet written.
- 2026-07-27 — **satellite changed from ESP32-S3 SuperMini to bare ESP-12E** on
  cost. Consequences: mic dropped from the roadmap entirely (§9), pin map moved
  to GPIO 4/5 with software I2C (§3.1), a programming jig and bring-up step
  added (§3.1, §7.1 step 0), and cross-family ESP-NOW promoted from a fallback
  caveat to the **primary link path** with an explicit API table (§4.1). The
  pad-side design in §5 is untouched by the swap — which is the intended
  property of §2.
