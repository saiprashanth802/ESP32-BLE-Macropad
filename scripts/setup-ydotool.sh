#!/usr/bin/env bash
# One-time setup for the companion's rewrite menu on Linux.
#
# The flow has to synthesise Ctrl+C / Ctrl+V into another application. Wayland
# forbids that between clients, so it goes through ydotool, which types on a
# /dev/uinput virtual keyboard the compositor cannot distinguish from real
# hardware. Creating that device needs permission, which is what this grants.
#
# Run as root:  sudo ./scripts/setup-ydotool.sh
# Then log out and back in so the group membership applies.
#
# See docs/LINUX_REWRITE_SETUP.md for what this enables and how to verify it.

set -euo pipefail

GROUP=ydotool
UDEV_RULE=/etc/udev/rules.d/80-uinput-ydotool.rules

if [[ $EUID -ne 0 ]]; then
    echo "This needs root (it installs packages and a udev rule)." >&2
    echo "Re-run as: sudo $0" >&2
    exit 1
fi

# The unit and group membership belong to the human, not to root. SUDO_USER is
# how we find them; without it we'd hand the daemon to root and the socket would
# be in the wrong runtime dir.
TARGET_USER=${SUDO_USER:-}
if [[ -z $TARGET_USER || $TARGET_USER == root ]]; then
    echo "Could not tell which user to set up (SUDO_USER is unset)." >&2
    echo "Run this via sudo from your normal login, not as a root shell." >&2
    exit 1
fi
TARGET_UID=$(id -u "$TARGET_USER")

echo "==> Setting up ydotool for user '$TARGET_USER' (uid $TARGET_UID)"

# ── 1. packages ─────────────────────────────────────────────────────────────
# The clipboard tool differs by session type; install for whichever the user is
# actually running, falling back to both if we can't tell (e.g. run from a TTY).
SESSION=$(sudo -u "$TARGET_USER" printenv XDG_SESSION_TYPE 2>/dev/null || true)
case "$SESSION" in
    wayland) CLIP=(wl-clipboard) ;;
    x11)     CLIP=(xclip xdotool) ;;
    *)       echo "    (session type unknown — installing both clipboard tools)"
             CLIP=(wl-clipboard xclip xdotool) ;;
esac

echo "==> Installing: ydotool ${CLIP[*]}"
if command -v dnf >/dev/null; then
    dnf install -y ydotool "${CLIP[@]}"
elif command -v apt-get >/dev/null; then
    apt-get update && apt-get install -y ydotool "${CLIP[@]}"
elif command -v pacman >/dev/null; then
    pacman -S --needed --noconfirm ydotool "${CLIP[@]}"
else
    echo "No supported package manager found — install ydotool and ${CLIP[*]} by hand." >&2
    exit 1
fi

# ── 2. group + uinput access ────────────────────────────────────────────────
if ! getent group "$GROUP" >/dev/null; then
    echo "==> Creating group '$GROUP'"
    groupadd "$GROUP"
fi

if ! id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$GROUP"; then
    echo "==> Adding '$TARGET_USER' to '$GROUP'"
    usermod -aG "$GROUP" "$TARGET_USER"
    NEEDS_RELOGIN=1
fi

# /dev/uinput is root-only by default. This is the one genuinely privileged part
# of the setup: anything in this group can synthesise input system-wide.
echo "==> Installing udev rule $UDEV_RULE"
cat > "$UDEV_RULE" <<EOF
# Let the '$GROUP' group open /dev/uinput, so ydotoold can run unprivileged.
# Installed by ESP32-BLE-Macropad/scripts/setup-ydotool.sh
KERNEL=="uinput", GROUP="$GROUP", MODE="0660", OPTIONS+="static_node=uinput"
EOF

udevadm control --reload-rules
udevadm trigger --name-match=uinput || true

# The module is usually built-in or autoloaded, but not always present at boot
# on a machine that has never used it.
modprobe uinput 2>/dev/null || true
echo uinput > /etc/modules-load.d/uinput.conf

# ── 3. ydotoold as a user service ───────────────────────────────────────────
# Deliberately a *user* unit: the daemon only needs the uinput device, and a
# root daemon holding a world-accessible socket would be a keylogger with extra
# steps. The explicit socket path is what the companion looks for.
UNIT_DIR="/home/$TARGET_USER/.config/systemd/user"
echo "==> Installing user unit $UNIT_DIR/ydotoold.service"
mkdir -p "$UNIT_DIR"
cat > "$UNIT_DIR/ydotoold.service" <<EOF
[Unit]
Description=ydotool daemon (virtual input device for MacroPad Deck)
Documentation=https://github.com/ReimuNotMoe/ydotool

[Service]
Type=simple
ExecStart=/usr/bin/ydotoold --socket-path=%t/.ydotool_socket --socket-perm=0600
Restart=always
RestartSec=2

[Install]
WantedBy=default.target
EOF
chown -R "$TARGET_USER":"$TARGET_USER" "/home/$TARGET_USER/.config/systemd"

echo "==> Enabling the unit for '$TARGET_USER'"
sudo -u "$TARGET_USER" XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
    systemctl --user daemon-reload
sudo -u "$TARGET_USER" XDG_RUNTIME_DIR="/run/user/$TARGET_UID" \
    systemctl --user enable --now ydotoold.service || true

echo
echo "==> Done."
if [[ -n ${NEEDS_RELOGIN:-} ]]; then
    echo "    LOG OUT AND BACK IN — '$TARGET_USER' was just added to '$GROUP',"
    echo "    and ydotoold cannot open /dev/uinput until that takes effect."
    echo
fi
echo "    Verify with:"
echo "      systemctl --user status ydotoold"
echo "      sleep 3; ydotool type 'hello'      # click into a text editor first"
