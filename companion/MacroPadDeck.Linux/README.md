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
| Firmware Wi-Fi join | `netsh` | **nmcli** |
| Tray | WinForms NotifyIcon | **Ayatana AppIndicator** (SNI) + GTK3 menu |
| Editor | WPF | **GTK3** (`GtkSharp`) |
| Autostart | Run registry key | `~/.config/autostart/*.desktop` |

## Requirements

**Display server:** X11. Foreground-follow and the window actions (snap/maximise/
next-monitor) use X11/EWMH and will not work under Wayland — the rest (pad link,
key launches, media, editor, firmware) still does. Run an Xorg session for full
parity.

**Runtime libraries / tools** (install names shown for Debian/Ubuntu):

- .NET 9 runtime (`dotnet-runtime-9.0`) — or publish self-contained
- `bluez` + a working BLE adapter, and the pad **paired once** so BlueZ knows it
- GTK3 runtime + **libayatana-appindicator3** (the tray). Fedora:
  `sudo dnf install gtk3 libayatana-appindicator-gtk3`
- `wmctrl`, `xdotool`, `xrandr` — window actions
- `NetworkManager` (`nmcli`) — firmware-update hotspot join
- An MPRIS-capable player for now-playing (Spotify, most browsers, native players)

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

## Not yet verified on hardware

This front-end was written and compiles/publishes clean, but the BlueZ, MPRIS,
X11 and GTK paths have **not been exercised on a real Linux box** from here.
First-run things to watch in `deck.log`:

- `ble: device … not known yet — scanning` → pair the pad once (`bluetoothctl`)
- `x11: XOpenDisplay failed` → you're on Wayland, not X11
- window actions doing nothing → install `wmctrl`/`xdotool`
