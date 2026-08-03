# Encoder Puck — Handoff

**Date:** 2026-08-03 · **Branch:** `linux-companion` · **Head `8ce6627`, pushed, tree clean.**

Supersedes the 2026-08-01 handoff, which described the ESP-12 as the active puck,
the C3 as dead, and the AS5600 as never wired. All three are now wrong.

---

## Status in one line

**The puck is finished.** Printed, assembled, and working on hardware — dial,
magnet, sleep cycle, spin-to-reboot and OTA all verified. What remains is a
better power board and one cosmetic check.

---

## Hardware inventory

| Thing | Identity | State |
|---|---|---|
| MacroPad v5 | STA `84:1F:E8:2B:33:48` · BLE `…:4A` · AP `…:49` | Working, current firmware |
| **ESP32-C3 puck** | MAC `88:56:A6:2B:ED:7C`, COM8 | **Active**, in the printed body |
| ESP-12E puck | MAC `E8:68:E7:81:9B:07`, COM15 | Spare. Protocol-synced, no OTA support |
| AS5600 | addr `0x36` | **Wired and working** |
| OLED | SSD1306 128×32 | **Dropped from the design** (`PUCK_OLED 0`) |

### The C3 is not dead

It was written off on 2026-07-31 after a lost decoupling cap left an apparent
0 Ω short on 3V3. That verdict was wrong, or the fault cleared: it enumerated,
flashed, and has been the working puck ever since. Ignore any older note saying
otherwise — including the ESP-12 power-board blocker, which is moot.

---

## Verified on hardware

- **Protocol v3 both directions** — mode switching, and the full volume path
  (Core Audio → companion → GATT → pad → ESP-NOW → puck) tracking in 2% steps
- **Rotation** — the AS5600 drives the whole chain; `PUCK_DIR_INVERT 0` is correct
- **Light sleep genuinely enters** — the C3 fully detaches from USB when no
  monitor is attached, which is the tell
- **Wake on movement** — the pad reads `LINKED` after a turn
- **Spin-to-reboot** and the **sleep cycle** behave on the real device
- **The puck OTA round trip** (2026-08-03), after four separate bugs — below
- **The printed enclosure** — the `(3)` STL set fits: the dial spins freely with
  no wobble, both counterpart halves mate, and the stack matches 24 mm. **The
  AS5600 magnet pocket worked first time**, despite being flagged throughout as
  the fussy dimension

## Not yet verified

- **The face-screen mode label** on the pad — flashed, never eyeballed
- **Any battery figure.** Every runtime number here is modelled, not measured.
  A meter in series with the cell is the only way to settle it, and it cannot be
  done over USB: an attached host suppresses CPU sleep by design

---

## Protocol v3

`docs/PUCK_PROTOCOL.md` is the source of truth. 9-byte struct, duplicated in
three sketches (Arduino folders cannot share a header), all marked `KEEP IN SYNC`.
**Not wire-compatible with v2** — the length check drops short packets before the
version check runs, so a mismatch is total silence with no error at either end.

The pad owns everything stateful: mode, and dial feel. The puck holds no
persistent copy — it boots on compiled defaults and adopts the pad's values on
the first `PK_STATE`.

**Sensitivity** rides as *detents per revolution*, not a raw threshold. Windows
moves volume 2% per `CONSUMER_VOL_UP`, so **50 ticks is the entire range** — which
makes the default of 50/turn "one turn, one sweep", and is the sanity check for
any retuning. `Settings → PUCK` has its own screen with live-pushing SPEED and
ACCEL sliders.

---

## Sleep architecture

Two states in `puck_encoder.ino`:

- **ACTIVE** — radio up, 10 ms encoder polling, 40 ms send coalescing
- **DOZE** — WiFi driver stopped, CPU light-sleeping in 50 ms slices, waking to
  poll the AS5600 over I2C. One full detent promotes back to ACTIVE

An unassociated STA cannot use WiFi modem sleep (no AP beacon to sync against),
so stopping the driver outright is the only way to put the radio down.

The AS5600 drops to **LPM2** while dozing (6.5 mA → 1.8 mA). Its 20 ms internal
polling is still faster than our 50 ms wake, so nothing is lost. `CONF` is
read-modify-written — that register also holds hysteresis and filtering.

`sleepGraceMs` is 30 s after a power-on reset so a serial monitor can attach, and
**0 after a software reset** — a watchdog reboot that then sat awake 30 s would
burn ~1 mAh each time.

### Recovery layers

| Trigger | Response |
|---|---|
| 5 min unlinked | `rebuildEspNow()` — full deinit/stop/start/init/re-peer |
| 30 min unlinked | `ESP.restart()` |
| 1 h unlinked | Deep sleep; spin ~10 s to wake |
| **Spin 2–3 turns while unlinked** | **Immediate reboot** |
| OTA with no upload for 5 min | Restart back into normal ESP-NOW |

**Spin-to-reboot exists because the puck is sealed in a printed body with no
reachable reset** — a hard spin *is* the reset button. Gated strictly on being
unlinked (30 s quiet), because spinning hard is completely ordinary during normal
volume use; that gate is the only thing making it safe.

Deep sleep wakes on a 3 s timer and needs `WAKE_MOVES_NEEDED` (3) consecutive
samples of ≥200 counts (~18°) before booting for real, so a knock or thermal
drift cannot wake it. **The AS5600 has no movement interrupt** — it is a plain
I2C angle sensor — which is why waking on rotation must be timer-poll-and-look.

---

## Power

**Current wiring is a stopgap:** the cell feeds the SuperMini's `5V` pin and its
onboard ME6211-class LDO makes 3V3. The CKCS boost module does charging only.

That change exists because the original chain — LiPo → 5 V boost → AMS1117 — had
two faults:

1. **~57% efficiency.** Boost at ~85%, then a linear regulator at 3.3/5 = 66%.
   Nearly half the pack became heat.
2. **Minimum-load shutdown.** Power-bank boost modules cut output below ~40–70 mA.
   A dozing puck draws single-digit mA, so the module decided nothing was plugged
   in and shut down. **There is no firmware recovery** — with the output off the
   C3 has no power at all, so it cannot pulse the module's button pad or signal
   anything. The interim workaround was a keepalive load pulse (10 s, then 5 s);
   the rewire removed the need and `DOZE_HELLO_MS` is back to 60 s.

⚠ **Two hazards of the direct tap.** Do not leave USB connected with the battery
on the `5V` pin — it ties to VBUS with no isolation on most SuperMini boards; fit
a Schottky (BAT54/1N5819) in the battery lead or unplug the cell to flash. And
confirm the LiPo has its own protection PCB, since tapping B+ may bypass whatever
the CKCS board provided. The C3's brownout detector (~2.98 V) is a crude backstop,
not protection.

### Planned board

One small PCB: **TP4056 (+DW01A/FS8205A protection) + SL7333 or AP2112K 3V3**,
removing the power-bank module entirely.

- `Rprog` 4 kΩ → 300 mA charge (0.5C on a 600 mAh cell)
- **1000 µF low-ESR** on the LDO output — the C3's TX peaks exceed a 250–300 mA
  LDO's rating and the cap rides them out
- **Verify the SOT-23-3 pinout against the vendor datasheet.** Three-pin LDO
  pinouts are not consistent between manufacturers
- **Run the AS5600's VCC from a GPIO.** It draws 6.5 mA, which the C3 sources
  easily, and it is what stops deep sleep being worthwhile today

TX power is capped at 11 dBm (`PUCK_TX_POWER_QDBM 44`) to keep peaks under the
LDO rating. **It resets on every `esp_wifi_start()`**, so it is re-applied in
`setupEspNow()`, `radioOn()` and `rebuildEspNow()`.

### Runtime model

Calibrated against the one real measurement — the original always-on build died
in 3 h on 600 mAh, which the model reproduces at ~200 mA.

| Configuration | Idle | Runtime |
|---|---|---|
| Always-on radio | ~200 mA | 3 h (measured) |
| + sleep firmware | ~39 mA | ~15 h |
| + BMS LEDs killed | ~15 mA | ~37 h |
| + battery-direct (current wiring) | ~2 mA | days |

⚠ **Deep sleep is barely a power win as wired.** C3 deep sleep is ~5 µA, but the
AS5600 still draws ~1.5 mA in LPM3 — the sensor dominates and the CPU being off
hardly registers. Its value today is behavioural. GPIO-powering the AS5600 is
what would turn it into a real saving.

---

## Gotchas that will recur

**A dozing C3 cannot be auto-flashed.** Light sleep powers down USB Serial/JTAG.
Windows keeps the cached descriptor so the port still *appears* and reports
Status OK, but opening it fails with *"A device attached to the system is not
functioning"* and esptool's DTR/RTS reset never lands. **Recovery: unplug, hold
BOOT, plug in, release.** A reset alone is often not enough — the physical
disconnect is what clears the stale device node. Set `PUCK_SLEEP 0` for bench
work if this gets tiresome.

**`RTC_DATA_ATTR` does not survive a software reset** — only deep sleep. The
bootloader reinitialises `.rtc.data` from flash. Use `RTC_NOINIT_ATTR`, and only
trust it when the reset reason is `ESP_RST_SW` or `ESP_RST_DEEPSLEEP`; it is
garbage after a power cycle.

**Never mix raw `esp_wifi_*` with the Arduino `WiFi` wrapper.** The doze cycle
uses `esp_wifi_stop()`/`esp_wifi_start()`; `WiFi.softAP()` goes through
`WiFiGeneric`, which tracks its own idea of whether the driver is started.
Changing that behind its back makes `softAP()` **silently no-op** — ESP-NOW tears
down but no AP appears. OTA reboots into AP mode via an RTC flag so the stack is
cold and coherent.

**`RADIO_LISTEN_MS` must outlast the pad's turnaround.** At 80 ms the pad's reply
routinely arrived after the radio was already down, silently dropping *every*
`PK_STATE` — mode, speed, accel and the OTA request with it. Now 400 ms with an
early exit the moment a reply lands. **If pad→puck settings ever appear to be
ignored, check this first.**

**`PUCK_OTA_PEND_MS` must outlast the puck's keepalive.** A dozing puck is quiet
by design, so "the puck went silent, therefore it acted on the request" is a
false inference — it dropped the latch ~10 s after the keypress, long before the
puck woke. Expiry is time-based only.

**The puck cannot wake a sleeping pad.** `esp_light_sleep_start()` powers down
WiFi on the pad too; packets during its sleep are lost.

**Pad STA MAC ≠ BLE address.** The puck targets `84:1F:E8:2B:33:48`, not the
`…:4A` the companion uses. Wrong one fails completely silently.

**Debug vs Release trap.** A running MacroPadDeck locks its Debug exe, so
`dotnet build` fails MSB3027 and only `-c Release` succeeds; relaunching Debug
then runs stale code and new features look missing.

### The OTA saga — four attempts, four different causes

All fixed, recorded because each presented identically ("no `Puck-Setup` AP"):

1. `RADIO_LISTEN_MS` 80 ms — pad→puck replies dropped wholesale
2. `WiFi.softAP()` silently no-opping from wrapper state desync
3. `RTC_DATA_ATTR` not surviving a software reset, which combined with the pad
   re-asserting its latch produced an **infinite reboot loop**
4. The pad clearing its latch ~10 s in, before a dozing puck could hear it

**Pull the puck's serial log first next time.** It identified causes 2, 3 and 4
within seconds each; re-triggering blind cost a round. The `[PAD]` diagnostic now
prints `ota=` so "did the flag arrive" is a glance, not a round trip.

---

## Build & flash

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" --libraries C:/Users/gsaip/Documents/Arduino/libraries --output-dir build_puck_c3 firmware/v5/puck_encoder
```

```bash
arduino-cli upload -p COM8 --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" --input-dir build_puck_c3 firmware/v5/puck_encoder
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:FlashFreq=40,UploadSpeed=115200,PartitionScheme=min_spiffs" --libraries C:/Users/gsaip/Documents/Arduino/libraries --output-dir build firmware/v5/macropad_v5
```

Pad OTA: `Settings → CONFIG`, join `MacroPad-Setup` / `macropad123`, then

```bash
curl -s -F "f=@build/macropad_v5.ino.bin;filename=firmware.bin" http://192.168.4.1/api/update
```

Puck OTA: `Settings → PUCK → OTA` on the pad, join `Puck-Setup` / `puck12345`,
then POST the puck binary to `192.168.4.1/api/update`. A dozing puck can take up
to `DOZE_HELLO_MS` to notice. **The AP disappearing is the success signal**, for
both devices.

**Always flash the C3 with `CDCOnBoot=cdc`** — the core defaults to routing
`Serial` to UART0, and the board then runs perfectly while printing nothing.

⚠ Joining `MacroPad-Setup` disconnects Windows from the real network, and it does
**not** auto-reconnect after the pad reboots. Restore with
`netsh wlan connect name="AirFiber-SvnePP"`.

Sizes: puck 79% flash / 11% RAM · pad 69% / 28%.

---

## CAD

`cad/stl/puck/` — revision `(3)`, pushed 2026-08-03, printed and fitted.

| File | Size | Role |
|---|---|---|
| `puck_dial.stl` | 50 × 50 × 12 mm | the knob |
| `puck_dial_counterpart_1.stl` | 73 × 55 × 17 mm | housing body |
| `puck_dial_counterpart_2.stl` | 73 × 55 × 7 mm | housing lid |

The counterparts share a footprint and stack to 24 mm with the dial seating into
them. Binary STL headers are all-zero, so no CAD metadata travels with the files —
**check export timestamps rather than trusting a folder suffix** when a new
revision arrives.

---

## Next steps

1. **Measure the battery** with a meter in series. Every number above is modelled.
2. **Build the power board** — TP4056 + LDO, and put the AS5600 on a GPIO so deep
   sleep is worth having.
3. **Eyeball the face-screen mode label** — the last unexercised feature.
4. Optional: **high-res scrolling**. Answered but not built — needs the HID
   Resolution Multiplier (usage `0x48`) in a logical collection plus a Feature
   report, since the plain wheel field's unit *is* one detent. Costs a Bluetooth
   off/on per host to clear the GATT wedge from adding Report ID 3.
