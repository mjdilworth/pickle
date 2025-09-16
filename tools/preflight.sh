#!/usr/bin/env bash
# Preflight environment checker for pickle (DRM+GBM+EGL+mpv) on Raspberry Pi / generic DRM systems.
# This script attempts to detect common issues that prevent successful non-root playback.
# It exits non-zero if a HARD failure is detected (e.g., missing DRM device, no mpv),
# but reports SOFT warnings for suboptimal conditions.

set -euo pipefail

PASS_ICON="[OK]"
WARN_ICON="[WARN]"
FAIL_ICON="[FAIL]"

fail=0
warn_count=0
warn() { echo -e "${WARN_ICON} $*"; warn_count=$((warn_count+1)); }
info() { echo -e "[INFO] $*"; }
pass() { echo -e "${PASS_ICON} $*"; }
fail() { echo -e "${FAIL_ICON} $*"; fail=1; }

# --- 1. Kernel / KMS overlay check (Raspberry Pi specific) ---
if grep -qE 'Raspberry Pi' /proc/device-tree/model 2>/dev/null; then
    CONFIG_TXT="/boot/firmware/config.txt"
    [ -f /boot/config.txt ] && CONFIG_TXT="/boot/config.txt"
    if grep -q '^dtoverlay=vc4-kms-v3d' "$CONFIG_TXT" 2>/dev/null; then
        pass "Full KMS overlay enabled (vc4-kms-v3d)"
    elif grep -q '^dtoverlay=vc4-fkms-v3d' "$CONFIG_TXT" 2>/dev/null; then
        warn "FKMS (fake KMS) overlay in use; prefer full KMS 'vc4-kms-v3d' for production"
    else
        warn "No vc4 KMS overlay line found in $CONFIG_TXT (expected: dtoverlay=vc4-kms-v3d)"
    fi
else
    info "Non-Raspberry Pi platform (skipping overlay check)"
fi

# --- 2. DRM devices ---
if ls /dev/dri/card* >/dev/null 2>&1; then
    cards=$(ls /dev/dri/card* 2>/dev/null | tr '\n' ' ')
    pass "DRM devices present: $cards"
else
    fail "No /dev/dri/card* devices found; kernel KMS driver not loaded"
fi

# --- 3. Group membership ---
uid=$(id -u)
user_groups=$(id -nG)
if echo "$user_groups" | grep -qw video; then
    pass "User is in 'video' group"
else
    warn "User not in 'video' group (add with: sudo usermod -aG video $USER)"
fi
if ls /dev/dri/renderD* >/dev/null 2>&1; then
    if echo "$user_groups" | grep -qw render; then
        pass "User is in 'render' group"
    else
        warn "Render node exists but user not in 'render' group (optional: sudo usermod -aG render $USER)"
    fi
fi

# --- 4. XDG_RUNTIME_DIR ---
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ]; then
    if [ "$(stat -c %u "$XDG_RUNTIME_DIR" 2>/dev/null)" = "$uid" ]; then
        pass "XDG_RUNTIME_DIR set and owned by user ($XDG_RUNTIME_DIR)"
    else
        warn "XDG_RUNTIME_DIR ($XDG_RUNTIME_DIR) not owned by current user; audio IPC may fail"
    fi
else
    warn "XDG_RUNTIME_DIR not set; audio (Pulse/PipeWire) likely to fail"
fi

# --- 5. mpv presence ---
if command -v mpv >/dev/null 2>&1; then
    pass "mpv CLI present (libmpv likely installed)"
else
    warn "mpv command not found; ensure libmpv-dev installed (sudo apt install libmpv-dev)"
fi

# --- 6. pkg-config checks ---
missing_pkgs=()
for p in mpv gbm egl glesv2 libdrm; do
    if ! pkg-config --exists "$p" 2>/dev/null; then
        missing_pkgs+=("$p")
    fi
done
if [ ${#missing_pkgs[@]} -eq 0 ]; then
    pass "All required pkg-config entries present"
else
    warn "Missing pkg-config entries: ${missing_pkgs[*]} (install dev packages)"
fi

# --- 7. Seat / logind status ---
if command -v loginctl >/dev/null 2>&1; then
    sid=${XDG_SESSION_ID:-}
    if [ -n "$sid" ]; then
        act=$(loginctl show-session "$sid" -p Active 2>/dev/null | cut -d= -f2)
        seat=$(loginctl show-session "$sid" -p Seat 2>/dev/null | cut -d= -f2)
        pass "logind session: id=$sid active=$act seat=$seat"
    else
        warn "No XDG_SESSION_ID; if on a bare tty this is OK, otherwise check loginctl"        
    fi
else
    warn "loginctl not found (no systemd-logind). Consider seatd if DRM master issues occur."
fi

# --- 8. Attempt DRM master dry-run (non-destructive) ---
can_master=1
for c in /dev/dri/card*; do
    if [ -r "$c" ] && [ -w "$c" ]; then
        # Try opening; actual drmSetMaster attempt would require ioctl; we just note rw perms.
        :
    else
        can_master=0
    fi
done
if [ $can_master -eq 1 ]; then
    pass "User has rw on DRM card nodes (likely can become DRM master on free VT)"
else
    warn "Insufficient rw permissions on some DRM card nodes (check udev rules / groups)"
fi

# --- 9. Optional: detect running compositor (basic heuristic) ---
if pgrep -x Xorg >/dev/null || pgrep -x Weston >/dev/null || pgrep -x sway >/dev/null; then
    warn "A compositor appears to be running; switch to a free VT for exclusive modeset"
else
    pass "No major compositor detected (good for fullscreen KMS)"
fi

# --- 10. Summarize ---
if [ $fail -ne 0 ]; then
    echo "\nSome HARD failures detected. Fix and re-run." >&2
    exit 1
fi

echo "\nPreflight completed with warnings=$warn_count (non-fatal)."
exit 0
