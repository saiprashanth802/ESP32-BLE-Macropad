# MacroPad Deck — Linux deploy & verify runbook

**Read this first if you're a Claude Code session on the Linux machine.** You are
picking up work started in a Windows session. Your job: get this Linux front-end
**running and verified on real hardware**, fix what the Windows session couldn't
test, then help commit.

> ## Verified on hardware — 2026-07-25 (Fedora 44, Wayland, fw v5 pad)
> Fixes made this session (all in `MacroPadDeck.Linux`):
> - **`MprisMediaSource.cs` — MPRIS now-playing FIX.** The old `IDBusProperties`
>   proxy (two-arg `GetAsync`/`GetAllAsync`) is rejected by Tmds.DBus 0.94 at
>   proxy-gen, so every player was silently skipped and nothing ever pushed.
>   Replaced with a typed `[DBusInterface("org.mpris.MediaPlayer2.Player")]
>   IMprisPlayer` + parameterless `GetAllAsync()`. Silent `catch` → deduped log.
> - **`BlueZBleLink.cs`** — added inbound-event trace `ble: evt op=0x.. len=.. [hex]`
>   at `OnValue` (hello/key/preset were invisible before; runbook assumed a hello line).
> - **`Program.cs` / `LinuxEditorWindow.cs`** — added a `--editor` launch flag
>   (`dotnet run -- --editor`) that opens the GTK editor without the tray, for
>   GNOME/Wayland before the AppIndicator extension is registered.
>
> **Verified working:** build (Core+Linux 0/0); #2 connect+hello (op 0x01, fw v5);
> #3 key→action (Firefox/GitHub/KiCad/EasyEffects/Feishin launched, op 0x02);
> #5 now-playing (`media: push … write=True` after the fix); #7 editor (via
> `--editor`); Feishin favourites; outbound writes; self-healing reconnect; tray
> lib loads. Pad runs faceMode "always" (eyes, no grid) — normal.
>
> **Still open:** #1 tray icon needs a GNOME re-login to register the AppIndicator
> ext (`gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com`).
> **N/A on Wayland:** #4 foreground-follow, #6 window-snap (X11 only; F44 has no
> GNOME Xorg session). **Untested:** #8 firmware (needs pad in CONFIG mode).
>
> Repo is checked out on an NTFS partition, so `git status` shows whole-repo
> CRLF/LF noise (`git diff -w` is empty for it); only `companion/` has real changes.

## What you're looking at

The companion app was refactored from a Windows-only app into three projects that
share a platform-agnostic core:

- `companion/MacroPadDeck.Core` — protocol, profiles, Feishin, DeckController,
  MediaPusher, and the platform interfaces. **Do not put OS-specific code here.**
- `companion/MacroPadDeck` — Windows front-end (WinRT/SMTC/Win32/WPF). Leave alone.
- `companion/MacroPadDeck.Linux` — **this project.** BlueZ, MPRIS, X11, GTK3.

Branch: **`linux-companion`** (already checked out on the Windows side; make sure
this clone is on it too). The whole thing **builds and publishes clean**, but the
BlueZ / MPRIS / X11 / GTK runtime paths were **never executed on Linux** — that's
what you're here to do. Protocol/hardware background is in the repo's
`docs/CONFIG_API.md` and the hardware handoff.

The pad's BLE address is its **base MAC + 2** = `84:1F:E8:2B:33:4A`
(config stores it as `841FE82B334A`).

## 1. Prerequisites

**Display server must be X11** for full parity. Check:

```bash
echo "$XDG_SESSION_TYPE"     # want: x11  (wayland = foreground-follow & window-snap won't work)
```

Install runtime deps. **Target machine is Fedora (GNOME):**

```bash
sudo dnf install -y dotnet-sdk-9.0 bluez NetworkManager wmctrl xdotool \
  gtk3 libayatana-appindicator-gtk3

# Tray on GNOME needs the AppIndicator/SNI host extension (KDE needs nothing):
sudo dnf install -y gnome-shell-extension-appindicator
gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com   # then log out / back in
```

Other distros: Debian/Ubuntu `apt install bluez network-manager wmctrl xdotool
gtk3 libayatana-appindicator3-1`; Arch `pacman -S bluez bluez-utils networkmanager
wmctrl xdotool gtk3 libayatana-appindicator dotnet-sdk`.

**Pair the pad once** so BlueZ knows it (the host-link command characteristic is an
encrypted write, so a bond is required — this is the single most likely thing to
trip up first-run):

```bash
bluetoothctl
  power on
  scan on            # wait until 84:1F:E8:2B:33:4A shows up, then:
  pair 84:1F:E8:2B:33:4A
  trust 84:1F:E8:2B:33:4A
  scan off
  quit
```

## 2. Build & run

```bash
cd companion/MacroPadDeck.Linux
dotnet run
```

First run writes `~/MacroPadDeck/profiles.json` (starter config) and logs to
`~/MacroPadDeck/deck.log`. Keep the log open in another pane:

```bash
tail -f ~/MacroPadDeck/deck.log
```

If `deviceAddress` in the json isn't `841FE82B334A`, fix it (no colons) and restart.

## 3. Verification checklist

Tick these off on hardware. The log prefix tells you which subsystem spoke.

| # | What to check | Expected | Log line |
|---|---|---|---|
| 1 | Tray icon appears | keyboard icon in the top bar (GNOME: needs the AppIndicator extension enabled) | `tray: no libayatana…` if the lib is missing |
| 2 | Pad connects | greyed top menu item → "● Pad online (fw v5…)" | `ble: UP`, then a hello |
| 3 | Key press runs action | press a DECK key → app launches/focuses | `action: …` |
| 4 | Foreground-follow | focus a mapped app → pad preset switches | (preset echo) |
| 5 | Now-playing | play music → title + progress on the pad | `mpris: push '…'` |
| 6 | Window snap | SnapL/SnapR/Max keys move the active window | — |
| 7 | Editor | tray → Open editor → change a colour → Save | `editor: … pushed` |
| 8 | Firmware | tray → Update firmware… (only with pad in CONFIG mode) | `firmware: …` |

## 4. Known risk areas (ranked) — where to look if something fails

These are the spots the Windows session flagged as untested. Debug against
`deck.log`, not by guessing.

1. **BLE notifications / no hello (`BlueZBleLink.cs`).** If you see `ble: UP` but no
   hello event and the pad never re-syncs: BlueZ may not have fired the pad's
   `onSubscribe`. On Windows this needed a CCCD off→on cycle per bond. If needed,
   try a StopNotifyAsync→StartNotifyAsync cycle after subscribing, or re-pair. Also
   confirm `GetDeviceAsync` found the device (`ble: device … not known yet` means it
   isn't paired/cached — go back to step 1's `bluetoothctl`).

2. **MPRIS property types (`MprisMediaSource.cs`).** D-Bus variant unboxing is the
   fragile bit: `mpris:length`/`Position` may come back as `long`/`int`/`ulong` and
   `xesam:artist` as `string[]`. If `mpris: EX …` appears or media never shows,
   log the actual runtime types of the offending values and adjust `ToLong`/casts.
   Test with `playerctl metadata` to see what your player exposes.

3. **X11 active-window read (`X11ForegroundWatcher.cs`).** If presets don't follow
   focus: `x11: XOpenDisplay failed` = you're on Wayland. Otherwise verify the
   property read returns a pid — compare against
   `xprop -root _NET_ACTIVE_WINDOW` and `xprop -id <win> _NET_WM_PID`. The
   format-32 read assumes a 64-bit client (reads one C-long per element).

4. **Tray via AppIndicator (`LinuxTray.cs`).** Uses `libayatana-appindicator3` by
   P/Invoke, with the GTK menu passed by native handle. If no icon appears:
   (a) `deck.log` shows `tray: no libayatana…` → the lib isn't installed
   (`dnf install libayatana-appindicator-gtk3`); (b) on GNOME the AppIndicator
   extension must be installed **and enabled** (`gnome-extensions list --enabled`);
   (c) the soname resolver tries `libayatana-appindicator3.so.1` then
   `libappindicator3.so.1` — verify one exists (`ldconfig -p | grep appindicator`).
   Note AppIndicator has no tooltip, so connection status shows as the greyed top
   menu item, and left-click opens the menu (no separate activate).

5. **Window actions (`LinuxActionEngine.cs`).** No-ops usually mean `wmctrl`/
   `xdotool`/`xrandr` aren't installed or you're on Wayland. `nextMonitor` parsing
   assumes `xrandr --listmonitors` format — verify on a multi-monitor setup.

6. **Firmware nmcli join (`LinuxFirmwareFlasher.cs`).** Uses SSID `MacroPad-Setup`
   password `macropad123`. If the join fails, do it once by hand in the NM applet;
   after that the saved connection makes it reliable.

## 5. When it works — commit

Confirm all three still build, then commit on `linux-companion`:

```bash
cd <repo root>
dotnet build companion/MacroPadDeck.Core && dotnet build companion/MacroPadDeck.Linux
git add companion/
git commit   # describe: cross-platform refactor + Linux (BlueZ/MPRIS/X11/GTK) front-end
```

Note: `companion/MacroPadDeck/Profiles.cs` had an unrelated uncommitted fix from a
prior Windows session that rode along on this branch — keep it in the commit unless
told otherwise. Record any hardware fixes you made (esp. items 1–3 above) in the
commit message so the next session knows what changed from the untested version.
