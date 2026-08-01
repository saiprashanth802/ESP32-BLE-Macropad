# Encoder Puck — Wiring Reference

Companion device for the MacroPad v5. A knob with a screen that talks to the pad
over ESP-NOW. Protocol: `docs/PUCK_PROTOCOL.md`. Firmware:
`firmware/v5/puck_encoder/puck_encoder.ino`.

## Bill of materials

| Part | Notes |
|------|-------|
| ESP32-C3 SuperMini (or ESP32-S3 mini) | Set `BOARD` at the top of the sketch |
| AS5600 breakout | I2C address `0x36`, fixed — not selectable |
| SSD1306 OLED, I2C | **As built: 0.91" 128×32.** Address `0x3C` (some are `0x3D`) |
| Diametrically magnetised magnet | 6×2.5 mm typical. **Radial magnets do not work.** |
| Momentary push button | Any SPST |
| Knob / shaft / bearing | Mechanical, your choice |

## Connections — ESP32-C3 SuperMini

Both I2C devices share one bus. Different addresses, so no conflict, and they
wire in parallel.

```
   ESP32-C3 SuperMini
   ┌───────────────┐
   │ 3V3 ──────────┼──┬──────────────┬─────────── AS5600 VCC
   │               │  │              └─────────── OLED   VCC
   │ GND ──────────┼──┼──┬───────────┬─────────── AS5600 GND
   │               │  │  │           └─────────── OLED   GND
   │               │  │  └───────────────────────  button leg B
   │ GPIO5 (SDA) ──┼──┼─────────────┬───────────── AS5600 SDA
   │               │  │             └───────────── OLED   SDA
   │ GPIO6 (SCL) ──┼──┼─────────────┬───────────── AS5600 SCL
   │               │  │             └───────────── OLED   SCL
   │ GPIO4 ────────┼──┼───────────────────────────  button leg A
   └───────────────┘
```

| Signal | C3 pin | S3 mini pin |
|--------|--------|-------------|
| I2C SDA | GPIO5 | GPIO35 |
| I2C SCL | GPIO6 | GPIO36 |
| Button | GPIO4 | GPIO2 |

**Why not the Arduino-default 8/9 on the C3:** GPIO2, 8 and 9 are strapping pins —
their level at boot selects boot mode. On the SuperMini specifically, GPIO8 also
drives the onboard LED, which hangs across the rail as a stray pullup. GPIO5/6/4 are
plain GPIOs with none of that. If you have already wired to 8/9 it will probably
work, but this is the first thing to suspect if the board boots erratically.

**Pull-ups:** most AS5600 and SSD1306 breakouts have 4.7 kΩ pull-ups fitted. Two
boards in parallel gives ~2.4 kΩ, which is fine at 400 kHz. Do not add more.

**Power:** both modules are 3V3. The AS5600 is not 5V tolerant on its I2C lines.

## Magnet placement

This is the part that most often goes wrong.

- The magnet must be **diametrically** magnetised (N and S across the diameter, not
  through the thickness). A radial/axial magnet reads as noise or a dead knob.
- Centre it on the AS5600 chip — not the board, the chip.
- Air gap **0.5–3 mm**. Closer is not better; saturation degrades the reading.
- The magnet must rotate with the knob and stay concentric. Wobble reads as jitter.

The firmware checks the AS5600 `MD` status bit at boot and every 5 s. **If the magnet
is missing or misplaced, the OLED shows `! NO MAGNET`** — you do not have to guess.

## First power-on

1. Flash the puck. **`CDCOnBoot=cdc` is not optional** — see below:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --libraries C:/Users/gsaip/Documents/Arduino/libraries --output-dir build_puck firmware/v5/puck_encoder
   ```
   ```bash
   arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --input-dir build_puck firmware/v5/puck_encoder
   ```
2. Open Serial at 115200. Boot prints the puck's own MAC, the pad MAC it targets,
   and whether the magnet was found.

> **USB CDC gotcha — cost us a silent-serial dead end on 2026-07-30.**
> The C3 core defaults to `cdc_on_boot=0`, which maps `Serial` to **UART0 on
> GPIO20/21**, not to the native USB port. Flash without `CDCOnBoot=cdc` and the
> board runs fine, enumerates fine, uploads fine — and prints absolutely nothing
> to the USB serial monitor. In the Arduino IDE this is
> *Tools → USB CDC On Boot → Enabled*. The pad's WROOM-32 has no such issue
> because its serial goes through a real CH340/CP2102 UART bridge.
3. The OLED should show `NO PAD` and a hollow link dot.
4. On the pad: **Settings → PUCK** to toggle it on. The pad brings up ESP-NOW
   immediately, and the cell reads `ON`.
5. Turn the knob. The pad toasts `PUCK LINKED`, its Settings cell changes to
   `LINKED`, and the puck's OLED header changes to `LINKED` with a filled dot.
6. Press SAVE on the pad so the setting survives a reboot.

## Troubleshooting

| Symptom | Cause |
|---------|-------|
| OLED blank, serial says `NOT WIRED` | Module is at `0x3D`, or SDA/SCL swapped, or no power |
| **Image cropped, doubled, or only half the screen used** | **`OLED_H` doesn't match the panel.** 0.91" = `32`, 0.96" = `64`. |
| `! NO MAGNET` on the OLED | Wrong magnet type, off-centre, or gap too large |
| OLED fine, `NO PAD` forever | Pad's PUCK setting is off, or `PAD_MAC` is wrong — see below |
| `PAD: NO BLE` | Link is fine; the pad just isn't connected to a host |
| `PAD: OFF` | Pad heard you but has the puck disabled in Settings |
| Knob jitters ±1 constantly | Magnet wobble, or `THRESHOLD` too low |
| Too sensitive / too coarse | `THRESHOLD` in the sketch — higher is coarser |
| Pad's BLE drops while spinning | Raise `SEND_INTERVAL_MS` (radio coexistence) |
| SCROLL mode does nothing | Expected — needs `PUCK_MOUSE_HID 1` on the pad |

**Wrong `PAD_MAC` is the classic failure.** The puck must target the pad's **station
MAC** (`84:1F:E8:2B:33:48`), not the BLE address the companion app uses
(`84:1F:E8:2B:33:4A`, base + 2). Same chip, two radios, two addresses. Using the BLE
one fails completely silently — ESP-NOW just sends into the void.

To confirm the pad's real station MAC, enable PUCK in Settings and watch its serial:
`[PUCK] listening, this pad's STA MAC: ...`
