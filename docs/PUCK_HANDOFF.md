# Encoder Puck + Face Work — Session Handoff

**Date:** 2026-08-01 · **Branch:** `linux-companion` · **Everything below is uncommitted.**

---

## Status in one line

All software is written, builds clean, and the ESP-NOW + volume + mode chain is
**verified on real hardware**. The only blocker is physical: the puck's
hand-built power board won't boot the ESP-12, though the ESP-12 itself is proven
good.

---

## Hardware inventory

| Thing | Identity | State |
|---|---|---|
| MacroPad v5 | STA MAC `84:1F:E8:2B:33:48` · BLE `…:4A` · AP `…:49` | Working, flashed with everything below |
| ESP32-C3 puck (original) | MAC `88:56:A6:2B:ED:7C` | **DEAD** — see below |
| ESP-12E puck (current) | MAC `E8:68:E7:81:9B:07` | Module proven good, circuit not booting |
| Programmer | Silicon Labs CP2102, **COM15** | Working, DTR/RTS auto-reset works |
| OLED | SSD1306 **128×32** (0.91"), addr `0x3C` | Was wired to the dead C3; not yet on the ESP-12 |
| AS5600 | addr `0x36` | **Never wired at any point** |

### The dead C3

While desoldering nearby, a decoupling cap came off. The board then read a hard
short (0 Ω) 3V3→GND with 5 V clean, and the LDO heated rapidly on power-up —
i.e. the regulator was alive and sourcing into a downstream short, most likely a
cracked MLCC. Cleaning the cap pads didn't clear it. Written off as not worth
component-level repair on a ~$2 board. **Don't suggest reviving it.**

---

## Verified working on hardware

- **Bidirectional ESP-NOW**, ESP8266 ↔ ESP32, protocol v2
- Pad `Settings → PUCK` reads `LINKED`
- Puck serial reported `[PAD] mode=VOLUME ble=1 scroll=0 off=0 vol=38%`
- **Volume path end to end**: Windows Core Audio → companion → GATT `0x8B` →
  pad → `PK_STATE` → puck. Confirmed tracking (27% on the slider showed as 27%
  on the puck).
- **Action enumeration**: companion logged `actions: fetched 86 builtin actions`
- **`PuckMode` key** bound and cycling `VOLUME ↔ ZOOM` on the puck (SCROLL
  correctly skipped, since `PUCK_MOUSE_HID` is 0)

## Not yet verified

- The AS5600 — never wired, so **no actual knob rotation has ever been tested**
- The OLED on the ESP-12 (it worked on the C3 at `0x3C`)
- The mouth + emote timing on the pad's screen (flashed but not eyeballed)
- Music-vs-video face gating (flashed, needs the companion restarted)

---

## ACTIVE BLOCKER — puck circuit won't boot

Power chain: **LiPo → BMS (5 V 2 A) → AMS1117-3.3 → ESP-12.**

### Ruled out

| Checked | Result |
|---|---|
| VCC at module | 3.3 V |
| `EN` / CH_PD | 3.3 V (10 k pullup fitted) |
| `GPIO15` | 0 V (10 k pulldown fitted) |
| `GPIO0` | was 2 V — **fault found**, bridged to 3V3, now 3.0 V |
| `RST` | wired, high |
| 220 µF output cap | fitted |
| BMS rating | 2 A @ 5 V, same module family already runs the macropad fine |
| **ESP-12 module itself** | **Boots fine in the programmer socket → module is GOOD** |

### The remaining suspicion

Everything above was measured **unloaded**. A supply reading a perfect 3.3 V at
meter currents can collapse entirely at 70 mA — true of a cold solder joint, a
marginal regulator, or a BMS cutting out, and none of it shows on an idle meter.

**The next test, not yet done:** put a **47 Ω** (≈70 mA) or **33 Ω** (≈100 mA)
½ W resistor across the 3V3 rail and watch the voltage.

- Holds 3.3 V → supply is fine, fault is the joints at the ESP-12 (reflow `VCC`
  and `GND` with fresh solder + flux; castellated pads read fine while being
  unable to pass current)
- Sags, or the BMS drops out → fault is upstream

**Also untried:** feed the AMS1117 from a plain USB 5 V source instead of the
BMS. That removes the BMS's under-load behaviour — invisible to a meter — from
the picture in one step.

### Note on the BMS

Power-bank style modules **auto-shut-down below a minimum load** (typically
40–70 mA), independent of their current rating. An ESP-12 idles around 70–80 mA,
right on that line. If it turns out to be this: a 100 Ω bleeder across the 5 V
keeps draw above the threshold, at the cost of ~50 mA of battery life. The
better long-term fix is dropping the 5 V boost entirely and using a buck-boost
straight to 3.3 V — the AMS1117 can't run from a LiPo directly (needs ~1.1 V of
dropout).

---

## Architecture built this session

### Puck protocol v2 — `docs/PUCK_PROTOCOL.md` is the source of truth

7-byte struct both directions, duplicated in three sketches (Arduino folders
can't share a header), all marked `KEEP IN SYNC`:

- `firmware/v5/macropad_v5/macropad_v5.ino` — pad / receiver
- `firmware/v5/puck_encoder/puck_encoder.ino` — ESP32-C3 puck
- `firmware/v5/puck_encoder_8266/puck_encoder_8266.ino` — ESP-12 puck (current)

**v2 is not wire-compatible with v1.** Both ends must match.

**The pad owns the dial mode.** The puck renders whatever `PK_STATE` says and
never self-changes; its (unwired) button only *requests* a cycle. One source of
truth, so a pad key and the puck button can't disagree.

**Volume is pushed, never estimated.** BLE HID volume is relative — the pad
sends up/down usages and is never told the level — so the companion reading
Windows Core Audio is the only way either device knows a real number. Tick-based
estimation was explicitly rejected: it desyncs the moment volume changes
anywhere else, and a confidently wrong number is worse than none.

### New host-link opcodes

| Op | Name | Purpose |
|---|---|---|
| `0x8B` | setVolume | True system volume, relayed to the puck |
| `0x8C` | getActions | Request a page of the pad's builtin action library |
| `0x04` | (event) actions | 8 entries per page, `[id][label 9]`, 11 pages for 86 actions |

`HCMD_MEDIA` flags gained **bit 2 = "this source is real music"** — no new
opcode, no length change.

### Companion additions

- `CoreAudioVolumeSource` — hand-rolled `IAudioEndpointVolume` interop, no new
  NuGet dependency
- `VolumePusher` — 150 ms poll, **writes only on change**, so a steady volume
  produces zero BLE traffic and zero log lines. Empty `volume:` lines in
  `deck.log` are correct, not a fault.
- `PadActions` — walks the action pages and caches. **The list is fetched from
  firmware, never duplicated in C#** — an 86-entry copy would drift silently and
  the failure mode is a key bound to the wrong action.
- Editor gained a **"Pad builtin action"** type. Bindings store the **id**, not
  the label, so renaming an action in firmware doesn't break existing keys.
- `DeckConfig.MusicSources` — allowlist, substring, case-insensitive, hot-reloads.
  Defaults cover feishin/spotify/foobar/etc. **Browsers deliberately excluded**:
  a browser session can't distinguish a song from a three-hour video. Empty list
  restores "everything is music".

### Face — mouth added

Three fields appended to `EyePose` (`mouthWPct`, `mouthCurve`, `mouthOpenPct`).
**Appended last on purpose** — every keyframe table uses aggregate init, so old
rows zero-fill and keep working. `mouthWPct == 0` means *default*, not *hidden*.

Because it lives in `EyePose`, the mouth inherits persona amplitude scaling,
mood blending and the emote easing automatically — no parallel system.

- Rendered as a **parabolic band**, column by column; softer than TFT_eSPI's arc
  primitives and trivially cheap next to the eye sprites
- Sprite 116×36 at y=194 — fits between the eye sprites (end y=175) and the
  now-playing strip (y=214) so nothing overlaps or leaves trails
- **Follows the glance** at 50% horizontal / 30% vertical, so the face moves as
  one piece (and the music bob reads as the head nodding)
- Eases at 0.26 vs the lids' 0.30 — trailing slightly reads as connected

**Emote timing:** `EMOTE_TIME_PCT` = **165**, applied by dividing elapsed time,
which stretches keyframe spacing *and* duration together. (Extending `durMs`
alone would only hold the last pose longer.) Personas gained a `timePct`:
Playful 85, Calm 105, Grumpy 115, Sleepy 145.

**`EM_GLANCE` is exempt from the stretch** — it fires on every keypress, and
slowing it makes the face feel laggy while typing.

**No mode toast on the pad** — the dial mode belongs on the puck's OLED. The one
exception kept: pressing `PuckMode` with the puck disabled toasts `PUCK IS OFF`,
because otherwise that key silently does nothing and reads as broken.

---

## Build & flash

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:FlashFreq=40,UploadSpeed=115200,PartitionScheme=min_spiffs" --libraries C:/Users/gsaip/Documents/Arduino/libraries --output-dir build firmware/v5/macropad_v5
```

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 --libraries C:/Users/gsaip/Documents/Arduino/libraries --output-dir build_puck8266 firmware/v5/puck_encoder_8266
```

```bash
arduino-cli upload -p COM15 --fqbn esp8266:esp8266:nodemcuv2 --input-dir build_puck8266 firmware/v5/puck_encoder_8266
```

Pad OTA: `Settings → CONFIG` on the pad, join `MacroPad-Setup` / `macropad123`,
then POST to `192.168.4.1`. **The AP vanishing is the success signal.**

```bash
curl -s -F "f=@build/macropad_v5.ino.bin;filename=firmware.bin" http://192.168.4.1/api/update
```

> A Windows WLAN profile named `MacroPad-Setup` was added this session to
> automate the OTA join. Harmless; delete if unwanted.

---

## Gotchas learned (these will recur)

**ESP32-C3 serial is silent by default.** The core defaults to `cdc_on_boot=0`,
routing `Serial` to UART0 on GPIO20/21 instead of native USB. The board uploads
and runs perfectly while printing nothing. **Always flash the C3 with
`CDCOnBoot=cdc` in the FQBN.** Cost a dead-end debug pass.

**Debug vs Release trap.** A running MacroPadDeck locks
`bin\Debug\…\MacroPadDeck.exe`, so `dotnet build` fails MSB3027 and only
`-c Release` succeeds. The user then relaunches **Debug**, which is stale, and
new features appear missing. Always check `Get-Process MacroPadDeck | Select
Path` and compare DLL timestamps before believing something is broken. Fix: fully
Exit the tray, build Debug, relaunch.

**Adafruit_SSD1306 `begin()` doesn't detect the panel.** It only fails on a
malloc error, so a missing OLED looks like a healthy one. Both puck sketches
probe the bus themselves and print a real I2C scan at boot.

**SSD1306 size is a build-time constant.** The controller can't report its own
panel size, so `OLED_H` mismatched to the hardware renders cropped with no error.
0.91" = 32, 0.96" = 64.

**Pad STA MAC ≠ BLE address.** The puck must target `84:1F:E8:2B:33:48`, not the
`…:4A` the companion uses. Wrong one fails completely silently.

**ESP8266 ESP-NOW needs `esp_now_set_self_role(ESP_NOW_ROLE_COMBO)`** — no ESP32
equivalent, and the link fails silently without it. Full API divergence table is
in `PUCK_PROTOCOL.md`.

**`WiFi.macAddress()` returns all zeros** until the driver is started — print it
*after* `WiFi.mode(WIFI_STA)`, not before.

**C3 pins are SDA=5 SCL=6 BTN=4**, deliberately not the Arduino-default 8/9:
GPIO2/8/9 are strapping pins and GPIO8 drives the SuperMini's onboard LED.
ESP-12 pins are **SDA=4 SCL=5 BTN=13** (GPIO 0/2/15 strapping, unusable).

**The puck can't wake a sleeping pad.** `esp_light_sleep_start()` powers down
WiFi; packets during sleep are lost and wake is GPIO-only. Press a pad key first,
or set SLEEP to OFF. Not fixable while keeping light sleep. The pad cycles
ESP-NOW on wake rather than trusting post-power-cycle radio state.

**`sizeof(FaceCfg)` changed** with the mouth fields, so the NVS size guard
rejects old `fcfg` blobs and compiled defaults load. Invisible in practice (eye
colour was already the default), but any config-UI tuning resets.

---

## Design decisions — don't re-litigate

- **Puck is dial + screen only, no button.** User wants it simple and compact.
  Button code remains in the sketch and is harmless unwired (GPIO reads HIGH).
- **Mode is driven from a pad key** (`PuckMode`, action id 132), not the puck.
- **USB-only flashing for the puck, no OTA.** The C3's default partition table
  already has dual OTA slots, but ESP-NOW (unassociated STA pinned to ch1)
  conflicts with every OTA transport. If ever wanted, the design is the pad's: a
  button gesture that stops ESP-NOW and raises a SoftAP.
- **Events are pad-only via BLE HID** — the v4 model. Deliberately not routed
  through the companion.
- **Scroll mode is behind `#define PUCK_MOUSE_HID 0`.** Volume (consumer) and
  zoom (Ctrl +/-, keyboard) need no HID descriptor change; scroll needs a
  Report ID 3, and adding it wedges Windows GATT discovery until a Bluetooth
  off/on toggle. Both flag states compile.

---

## Next steps

1. **Load-test the 3V3 rail** (47 Ω / 33 Ω) — the one diagnostic not yet run
2. Or feed the AMS1117 from USB 5 V to eliminate the BMS
3. If the supply holds, reflow `VCC`/`GND` at the ESP-12
4. Wire the OLED to the ESP-12 — `SDA=GPIO4`, `SCL=GPIO5` (**not** the C3's 5/6)
5. Wire the AS5600 — same two lines, plus `DIR`→GND. Diametric magnet, centred
   on the chip, 0.5–3 mm gap
6. Restart the companion to pick up the music-vs-video gating
7. Eyeball the mouth on the pad, especially whether it looks cramped against the
   now-playing strip (`MOUTH_CY` / `MOUTH_SPR_H` are the knobs)
8. Consider the offered **GPIO2 heartbeat blink** — on a headless board with no
   serial, it turns "is it alive?" into a glance
9. Commit — nothing from this session is committed
