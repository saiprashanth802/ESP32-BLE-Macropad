# CAD — Print Files

All STL files are export-ready. Slice and print as-is.

## 📁 Folder Structure

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
