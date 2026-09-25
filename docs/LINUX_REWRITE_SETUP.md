# Rewrite menu on Linux — one-time setup

The local-model rewrite menu (see [REWRITE_MENU.md](REWRITE_MENU.md)) reads the
selection out of whatever app is focused and pastes the rewrite back over it.
On Windows that is a single `SendInput` call. On Linux it needs two things the
distro does not set up for you.

Everything else in the companion works without this. If you skip it, the pad
reports `NOTHING SELECTED` when you press a `write` key and the rest of the app
carries on normally.

## 1. A local model server

The flow talks to Ollama on `http://localhost:11434` by default (override with
`llmUrl` in `styles.json`).

```bash
sudo dnf install -y ollama            # or your distro's package
sudo systemctl enable --now ollama
ollama pull qwen2.5:7b-instruct       # whatever `model` in styles.json names
```

`styles.json` lives next to `profiles.json` — the tray's **Edit styles.json**
opens it. The tray's **Load write model** / **Unload write model** items let you
park the model in VRAM before a writing session and hand it back before a game.

## 2. Key injection that Wayland accepts

The companion has to synthesise Ctrl+C and Ctrl+V *into another application*.

- **X11** allows this through XTEST, which is what `xdotool` drives.
- **Wayland does not.** A client cannot inject input into another client, by
  design. No compositor flag changes this.

The way through on both is [ydotool](https://github.com/ReimuNotMoe/ydotool),
which writes to a `/dev/uinput` virtual device. The compositor sees an ordinary
USB keyboard and cannot tell the difference — which is exactly why it needs
permission to create one.

```bash
sudo ./scripts/setup-ydotool.sh
```

That script is short and worth reading before running as root. It:

1. installs `ydotool`, plus `wl-clipboard` (Wayland) or `xclip` (X11);
2. creates a `ydotool` group and adds you to it;
3. drops a udev rule giving that group access to `/dev/uinput`;
4. installs a **user** systemd unit for `ydotoold` and starts it.

The daemon runs as your user, not as root — it only needs the uinput device,
and a root daemon holding a world-writable socket would be a keylogger with
extra steps.

**Log out and back in** afterwards so the new group membership applies. Verify:

```bash
systemctl --user status ydotoold      # active (running)
id -nG | tr ' ' '\n' | grep ydotool   # ydotool
```

Then test injection into a text editor — click into it first, so it has focus:

```bash
sleep 3; ydotool type "hello from ydotool"
```

If nothing is typed, the daemon is not reachable. Check `YDOTOOL_SOCKET`:
packagings disagree on where the socket lives. The companion honours an explicit
`YDOTOOL_SOCKET` and otherwise looks for `$XDG_RUNTIME_DIR/.ydotool_socket`,
which is what the unit installed by the script uses.

## How the Wayland flow differs from Windows

Two behavioural differences fall out of the compositor's restrictions. Both are
in `LinuxTextCapture` and `GtkRewritePreview`.

**The preview window never takes focus.** Wayland offers no way to hand focus
back to the app the selection came from, so instead focus is never taken away:
the preview is mapped non-focusable and the target app keeps the keyboard for
the whole flow. This suits the feature — WriteFlow's premise is that the pad is
the input device and the monitor is only the display.

**The preview is therefore read-only under Wayland.** Editing needs focus.
Accept the rewrite as generated, press REGEN for another pass, or paste and edit
in place afterwards. Under X11 the window is focusable and editable, matching
the Windows build, and `Ctrl+Enter` / `Esc` work.

If you click the preview window anyway, it will take focus, and the companion
will *refuse to paste* rather than fire Ctrl+V at whatever is focused now — the
pad reports `PASTE FAILED`. Pasting a rewritten paragraph into the wrong
application is far worse than doing nothing.

## Clipboard

The selection travels via the clipboard, so the companion briefly overwrites it
and puts the old value back afterwards. Only the text flavour is preserved — a
clipboard holding an image or files can't be round-tripped this way.

Under Wayland `wl-copy` forks a small server to own the selection, so you may
see a stray `wl-copy` process after a rewrite. That is normal; it exits when
something else claims the clipboard.
