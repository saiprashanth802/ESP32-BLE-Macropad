# MacroPad Deck — the companion app

The pad works on its own. The companion is what turns it into a Stream Deck: it
launches and focuses apps from a key press, pushes now-playing to the pad's
screen, follows the foreground window to switch presets, feeds the encoder puck
a real volume number, drives the local-LLM rewrite menu, and gives you a GUI
editor instead of hand-editing JSON.

Two front-ends over one shared core:

| Project | Target | What it is |
|---|---|---|
| `MacroPadDeck.Core` | `net9.0` | Wire protocol, profiles, Feishin client, `DeckController`, the platform interfaces. **No OS-specific code goes here.** |
| `MacroPadDeck` | `net9.0-windows10.0.19041.0` | Windows front-end — WinRT/SMTC, Core Audio, Win32, WPF |
| `MacroPadDeck.Linux` | `net9.0` | Linux front-end — BlueZ, MPRIS, X11/Wayland, GTK3 |

`GattSpike/` is a throwaway BLE probe, not part of the solution.

---

## 1. Install the .NET 9 SDK

The SDK, not just the runtime — you are building from source.

**Fedora / RHEL**
```bash
sudo dnf install -y dotnet-sdk-9.0
```

**Debian / Ubuntu**
```bash
sudo apt update && sudo apt install -y dotnet-sdk-9.0
```
On older releases that package does not exist; add Microsoft's feed first
(`packages-microsoft-prod.deb` from `packages.microsoft.com`) or use the
[install script](https://dot.net/v1/dotnet-install.sh).

**Arch**
```bash
sudo pacman -S dotnet-sdk
```

**Windows** — `winget install Microsoft.DotNet.SDK.9`, or the installer from
<https://dotnet.microsoft.com/download/dotnet/9.0>.

**Any OS, no root** — `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 9.0`,
then put `~/.dotnet` on your `PATH`.

Verify — the first line of output must start with `9.`:

```bash
dotnet --list-sdks
```

## 2. Install the platform dependencies

### Linux

```bash
# Fedora
sudo dnf install -y bluez NetworkManager gtk3 libayatana-appindicator-gtk3 \
                    wmctrl xdotool

# Debian / Ubuntu
sudo apt install -y bluez network-manager gtk3 libayatana-appindicator3-1 \
                    wmctrl xdotool

# Arch
sudo pacman -S bluez bluez-utils networkmanager gtk3 libayatana-appindicator \
               wmctrl xdotool
```

On GNOME the tray needs the AppIndicator host extension — KDE needs nothing:

```bash
sudo dnf install -y gnome-shell-extension-appindicator
gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com
```

`wmctrl` and `xdotool` are X11-only; window-snap actions and foreground-follow
do nothing under Wayland without them, and foreground-follow needs a further
step there (see §6).

### Windows

Nothing beyond the SDK. WinRT, SMTC and Core Audio ship with the OS.

## 3. Pair the pad — do this before the first run

The host-link command characteristic is an **encrypted** write, so a BLE bond
must already exist. This is the single most common first-run failure: the app
connects, then every write silently fails.

```bash
bluetoothctl
  power on
  scan on          # wait for the pad to appear, then:
  pair  <MAC>
  trust <MAC>
  scan off
  quit
```

The pad advertises on its **base MAC + 2**. On Windows, pair it once through
Settings → Bluetooth as an ordinary keyboard.

## 4. Build and run

**Build the project, not the solution.** `dotnet build MacroPadDeck.sln` fails
on Linux with `error NETSDK1100: To build a project targeting Windows on this
operating system…` — the Windows front-end cannot be built there at all.

**Linux**
```bash
cd companion/MacroPadDeck.Linux
dotnet run
```

**Windows**
```powershell
cd companion\MacroPadDeck
dotnet run
```

First run writes `~/MacroPadDeck/profiles.json` (`%USERPROFILE%\MacroPadDeck\`
on Windows) and logs to `deck.log` beside it. Keep the log open — it is how you
debug everything:

```bash
tail -f ~/MacroPadDeck/deck.log
```

Set `deviceAddress` in `profiles.json` to your pad's address **without colons**
and restart.

Useful flags:

```bash
dotnet run -- --editor      # open the GTK editor directly, no tray
```

That is the way in on GNOME/Wayland before the AppIndicator extension is
registered, since without it there is no tray icon to click.

## 5. Install it properly

Publish a self-contained single file — no SDK needed on the target, and nothing
to break when a runtime updates.

**Linux**
```bash
cd companion/MacroPadDeck.Linux
dotnet publish -c Release -r linux-x64 --self-contained true \
               -p:PublishSingleFile=true -o out
./out/MacroPadDeck.Linux
```

**Windows**
```powershell
cd companion\MacroPadDeck
dotnet publish -c Release -r win-x64 --self-contained true `
               -p:PublishSingleFile=true -o out
```

> **Stop the running instance before republishing.** A live process holds the
> executable open and the publish fails, or worse, half-succeeds.

To start it at login on Linux, drop a `.desktop` file in
`~/.config/autostart/` pointing at the published binary.

## 6. Optional — the local-LLM rewrite menu

Select text anywhere, press a pad key, pick a style, and the rewrite is pasted
back over the selection. It needs a local model server, and on Linux it needs
one more thing.

```bash
# a model server
sudo dnf install -y ollama          # or your distro's package
sudo systemctl enable --now ollama
ollama pull qwen3:8b                # whatever `model` in styles.json names
```

On Linux the app has to synthesise Ctrl+C and Ctrl+V *into another application*.
Wayland forbids that by design and no compositor flag changes it, so both X11
and Wayland go through `ydotool` and a `/dev/uinput` virtual device:

```bash
sudo ./scripts/setup-ydotool.sh     # from the repo root; read it first
# then log out and back in for the new group to apply
```

Skip this and the rest of the app is unaffected — pressing a rewrite key just
reports `NOTHING SELECTED`.

Full guides: [`../docs/REWRITE_MENU.md`](../docs/REWRITE_MENU.md) ·
[`../docs/LINUX_REWRITE_SETUP.md`](../docs/LINUX_REWRITE_SETUP.md) ·
model choice and the fine-tune that did not ship:
[`../docs/FIX_MODEL_FINETUNE.md`](../docs/FIX_MODEL_FINETUNE.md)

## 7. Verify it works

| # | Check | Expected | Log line |
|---|---|---|---|
| 1 | Tray icon | keyboard icon in the top bar | `tray: no libayatana…` if the lib is missing |
| 2 | Pad connects | top menu item reads "● Pad online (fw v5…)" | `ble: UP`, then a hello |
| 3 | Key press | an app launches or focuses | `action: …` |
| 4 | Foreground-follow | focusing a mapped app switches preset | (preset echo) |
| 5 | Now-playing | title and progress appear on the pad | `mpris: push '…'` / `media: push …` |
| 6 | Editor | tray → Open editor → change a colour → Save | `editor: … pushed` |
| 7 | Firmware update | tray → Update firmware… (pad in CONFIG mode only) | `firmware: …` |

## Troubleshooting

**`ble: UP` but no hello, and nothing the app writes lands.** The bond is
missing or BlueZ never fired the pad's subscribe. Re-pair (§3); if it persists,
cycle notifications off and on after subscribing.

**`ble: device … not known yet`.** BlueZ has no cached record — it is not
paired. Back to §3.

**No tray icon.** In order: is `libayatana-appindicator3` installed
(`ldconfig -p | grep appindicator`), is the GNOME extension **enabled**
(`gnome-extensions list --enabled`), and did you log out and back in. Meanwhile
use `--editor`.

**Presets do not follow focus.** `x11: XOpenDisplay failed` means you are on
Wayland; that watcher is X11-only. The Wayland path polls the Focused Window
D-Bus GNOME extension instead, and either way it stays inert until each
profile's `appMatch` in `profiles.json` is actually filled in — an empty
`appMatch` matches nothing.

**Window-snap keys do nothing.** `wmctrl` / `xdotool` / `xrandr` missing, or
Wayland.

**Media never appears on Linux.** Check what your player exposes with
`playerctl metadata`; MPRIS variant unboxing is the fragile part.

**Firmware update cannot join the pad's hotspot.** SSID `MacroPad-Setup`,
password `macropad123`. Join it once by hand; the saved connection makes it
reliable afterwards.
