# MacroPad Deck — Linux

The Linux front-end of the MacroPad companion. It shares all wire-protocol,
profile, Feishin and orchestration logic with the Windows build via
`MacroPadDeck.Core`; only the platform edges differ.

| Concern | Windows | Linux (this project) |
|---|---|---|
| Pad link (BLE GATT) | WinRT | **BlueZ** over D-Bus (`Linux.Bluetooth`) |
| Now-playing | SMTC | **MPRIS** over D-Bus (`Tmds.DBus`) |
| Foreground-follow | `SetWinEventHook` | **X11** `_NET_ACTIVE_WINDOW` polling |
| Key actions / window snap | Win32 | **wmctrl / xdotool / xdg-open** |
| Master volume (puck readout) | Core Audio | **pactl** (PulseAudio / PipeWire) |
| Rewrite: key injection | `SendInput` | **ydotool** (uinput) — X11 *and* Wayland |
| Rewrite: clipboard | WinForms Clipboard | **wl-clipboard** / **xclip** |
| Rewrite: preview window | WPF | **GTK3**, non-focusable under Wayland |
| Firmware Wi-Fi join | `netsh` | **nmcli** |
| Tray | WinForms NotifyIcon | **Ayatana AppIndicator** (SNI) + GTK3 menu |
| Editor | WPF | **GTK3** (`GtkSharp`) |
| Autostart | Run registry key | `~/.config/autostart/*.desktop` |

Everything else — pad protocol, profiles, presets, Feishin, the encoder puck's
volume stream, the pad-fetched action picker, and the whole rewrite state
machine — is shared `MacroPadDeck.Core` code, identical on both platforms.

## Requirements

**Display server:** works under both, with one gap. Foreground-follow (profile
auto-switching) and the window actions (snap/maximise/next-monitor) use
X11/EWMH, so they are inert under Wayland; everything else — pad link, key
launches, media, volume, editor, firmware, and the rewrite menu — works on
either. Run an Xorg session if you want profile auto-follow.

**Runtime libraries / tools** (install names shown for Debian/Ubuntu):

- .NET 9 runtime (`dotnet-runtime-9.0`) — or publish self-contained
- `bluez` + a working BLE adapter, and the pad **paired once** so BlueZ knows it
- GTK3 runtime + **libayatana-appindicator3** (the tray). Fedora:
  `sudo dnf install gtk3 libayatana-appindicator-gtk3`
- `wmctrl`, `xdotool`, `xrandr` — window actions
- `pactl` (`pulseaudio-utils` / `pipewire-pulse`) — master volume for the puck
- `NetworkManager` (`nmcli`) — firmware-update hotspot join
- An MPRIS-capable player for now-playing (Spotify, most browsers, native players)
- **Rewrite menu only:** `ydotool` + a running `ydotoold`, and `wl-clipboard`
  (Wayland) or `xclip` (X11), plus a local Ollama. One-time setup:
  `sudo ./scripts/setup-ydotool.sh` — see
  [docs/LINUX_REWRITE_SETUP.md](../../docs/LINUX_REWRITE_SETUP.md). Skipping this
  leaves every other feature working; `write` keys just report
  `NOTHING SELECTED`.

> **GNOME note (Fedora Workstation default):** the tray uses AppIndicator/SNI,
> which GNOME shows only via an extension. Install and enable it once:
> ```bash
> sudo dnf install gnome-shell-extension-appindicator
> gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com   # then log out/in
> ```
> KDE Plasma shows SNI tray icons natively — no extension needed.

## Build & run

```bash
cd companion/MacroPadDeck.Linux
dotnet run
```

Self-contained single build to copy to another machine:

```bash
dotnet publish -c Release -r linux-x64 --self-contained true \
  -p:PublishSingleFile=true -o out
./out/MacroPadDeck.Linux
```

## Configuration

Same as Windows: `~/MacroPadDeck/profiles.json`, hot-reloaded on save, log at
`~/MacroPadDeck/deck.log`. First run writes a starter config (edit the `firefox`/
`code` sample bindings in the DECK preset to taste). Set `deviceAddress` to the
pad's BLE address (base MAC **+2**). Edit via the tray's **Open editor**, or
**Edit profiles.json**, or any text editor.

Rewrite styles live alongside it in `~/MacroPadDeck/styles.json` (tray → **Edit
styles.json**), same format and defaults as Windows.

The editor's key types include `padAction` — the pad's own builtin actions,
fetched live over the link, so the picker always matches the firmware actually
flashed — and `write`, which opens the rewrite menu.

## Troubleshooting

Things to watch for in `deck.log`:

- `ble: device … not known yet — scanning` → pair the pad once (`bluetoothctl`)
- `x11: XOpenDisplay failed` → you're on Wayland; profile auto-follow and window
  actions are inert, everything else still works
- window actions doing nothing → install `wmctrl`/`xdotool`
- `volume: pactl …` → no `pactl` on PATH; the puck falls back to showing mode only
- `capture: could not synthesise Ctrl+C — is ydotoold running?` →
  `systemctl --user status ydotoold`, and check you logged out and back in after
  `setup-ydotool.sh` added you to the `ydotool` group
- `capture: preview window took focus` → you clicked the preview; under Wayland
  that costs the paste target, so the paste is refused rather than misaimed
