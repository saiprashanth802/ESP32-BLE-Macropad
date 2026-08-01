# CAD — Print Files

All STL files are export-ready. Slice and print as-is.

## v5 — the built pad (current)

These are the parts in the photos in the main README — 12-key 4×3 layout with a
2" IPS TFT above the grid. **Use these for a v5 build**; the folders below are
the earlier encoder-based design.

```
cad/stl/v5/
├── v5_case_bottom.stl        ← bottom shell (ESP32 + wiring bay)
├── v5_case_top_plate.stl     ← top plate (key openings + display shelf)
├── v5_display_mount.stl      ← display mount (2" IPS TFT carrier)
├── v5_switchplate.stl        ← switch plate (12 × MX cutouts, 19.05mm pitch)
└── v5_keycap_low_profile.stl ← low-profile keycap, print 12×
```

**Stack-up, bottom to top:** case bottom → switch plate (switches clip in) →
top plate → display mount carrying the TFT on the raised shelf. M3 screws pass
through the corner bosses to hold the sandwich together.

### Print settings (as actually printed)

| Part | Qty | Layer | Walls | Infill |
|------|-----|-------|-------|--------|
| Case parts, switch plate | 1 each | 0.2 mm | 3 | 30% adaptive cubic |
| `v5_keycap_low_profile.stl` | 12 | 0.12 mm | 3 | 100% |

White PLA for the case; the build in the photos alternates black and white
keycaps in a checkerboard. The fine layer height plus solid infill is what gives
the keycaps their crisp concentric spiral top surface — the pattern is the
slicer's top-surface path, so **keep the 100% infill and 0.12 mm layers if you
want that look**. The assembled unit is lit by a blue LED strip behind the
left-hand vent slots.

## Encoder puck (wireless dial)

The standalone ESP-NOW knob — see `docs/PUCK_PROTOCOL.md` for the firmware side
and `hardware/puck_wiring.md` for the electronics.

```
cad/stl/puck/
├── puck_dial.stl               ← the knob itself        50 × 50 × 12 mm
├── puck_dial_counterpart_1.stl ← housing body           73 × 55 × 17 mm
└── puck_dial_counterpart_2.stl ← housing lid / plate    73 × 55 ×  7 mm
```

The two counterpart parts share a 73 × 55 mm footprint and stack to 24 mm; the
50 mm dial seats into them. Exported together on 2026-07-31 — `puck_dial.stl`
and this counterpart pair are a **matched set**, so don't mix them with an
earlier counterpart export.

> ⚠ **Not yet printed or test-fitted.** These are the CAD exports as designed;
> no tolerance check against the real AS5600 + magnet stack has happened yet.
> Expect to iterate on the magnet pocket in particular — the AS5600 wants a
> diametrically magnetised magnet centred on the chip at a **0.5–3 mm** gap, and
> that gap is the one dimension the sensor is genuinely fussy about.

## Earlier design (v2/v3, encoder-based)

```
cad/stl/
├── enclosure/
│   ├── enclosure_top.stl    ← top shell  (buttons + display + encoder cutouts)
│   └── enclosure_base.stl   ← base tray  (PCB + 18650 cell bay)
├── keycap/
│   ├── keycap_v1.stl        ← keycap body (main variant, high detail)
│   ├── keycap_v2.stl        ← keycap variant 2
│   ├── keycap_v3.stl        ← keycap variant 3 (minimal)
│   └── keycap_v4.stl        ← keycap variant 4
└── knob/
    └── encoder_knob.stl     ← EC12 encoder knob (GEIST KLOTZ v4, press-fit D-shaft)
```

## 🖨 Print Settings

| Part                  | Qty | Layer  | Walls | Infill     | Supports |
|-----------------------|-----|--------|-------|------------|----------|
| enclosure_top.stl     | 1   | 0.2mm  | 3     | 20% gyroid | Yes (TFT overhang) |
| enclosure_base.stl    | 1   | 0.2mm  | 3     | 20% gyroid | No |
| keycap_v*.stl         | 8x  | 0.15mm | 3     | 15%        | No |
| encoder_knob.stl      | 1   | 0.15mm | 4     | 20%        | No |

- Material: PLA (PETG also works for keycaps)
- Bed adhesion: Brim for enclosure parts, Skirt for keycaps/knob
- Keycaps are press-fit over 12x12mm tact switches — no hardware needed
- Encoder knob is press-fit on the EC11/EC12 6mm D-shaft

## 🔧 Assembly Notes

- Print 8 keycaps (same file, plate them in your slicer)
- Enclosure top and base friction-fit or secure with M3 screws through corner bosses
- The TFT module slots into the rectangular window in enclosure_top.stl
- Encoder shaft passes through the circular hole beside the TFT window

## 🛠 CAD Source

Modelled in Onshape (browser-based, free for public projects).
Project name: steam_deck

## v2 -> v3 CAD Improvements Planned
- [ ] Snap-fit lid clips (replace friction fit)
- [ ] Internal standoffs for PCB screw mounting
- [ ] Tighter TFT window clearance
- [ ] Dedicated 18650 retainer in base tray
- [ ] Cable channel for battery wires
