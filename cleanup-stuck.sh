#!/bin/bash
# Recovery cleanup for the 5 legacy cockblock files left +i (immutable) after the
# first uninstall-previous.sh run hit a cap-drop + missing-updater ordering bug.
#
# Why these are stuck: the first run removed /opt/cockblock (which held the
# legacy gated updater cockblock-updat) BEFORE trying to clear +i on the apt
# pin / apparmor profiles / apparmor override. Your login session is cap-dropped
# (CAP_LINUX_IMMUTABLE removed), so a plain `sudo chattr -i` is denied by the
# kernel, and the legacy updater was already gone.
#
# This clears them WITHOUT a reboot by:
#   * systemd-run: spawns a transient root service. PID 1 (the system manager)
#     keeps the full bounding set (CAP_LINUX_IMMUTABLE included) — the cap-drop
#     only hit user-session spawners (lightdm/getty/user@). The transient
#     service therefore HAS the cap.
#   * the NEW updater bpf/ChickenBlockerUpd: uses the raw FS_IOC_SETFLAGS ioctl
#     (NOT the /usr/bin/chattr binary), so it is NOT confined by the (still
#     loaded, legacy) AppArmor chattr profile that denies the apt-pin path. The
#     legacy BPF setflags gate was detached (its bpffs pins were removed), so
#     there is no comm gate to match against either.
#
# After this, finish the ChickenBlocker install:
#   sudo bpf/install.sh          # deploy BPF artifacts to /opt/ChickenBlocker/bpf + load new gate
#   sudo ./apparmor/install.sh   # deploy new AppArmor profiles + apt pin + immutable lockdown
#   sudo ./update.sh             # refresh protected files via the new updater
# (then re-install the hardening layers you want: make install-update-service,
#  make install-cap-drop, make install-session-capdrop, and reboot)
#
# Run as root:  sudo ./cleanup-stuck.sh
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "Must run as root (use: sudo $0)" >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
UPDATER="$REPO_ROOT/bpf/ChickenBlockerUpd"

STUCK=(
  /etc/apt/preferences.d/99-cockblock-no-browsers
  /etc/systemd/system/apparmor.service.d/override.conf
  /etc/apparmor.d/usr.bin.chattr
  /etc/apparmor.d/usr.bin.apparmor_parser
  /etc/apparmor.d/shell-bpf
)

if [ ! -x "$UPDATER" ]; then
  echo "Building the new updater ($UPDATER) ..." >&2
  (cd "$REPO_ROOT/bpf" && cc -O2 -g -Wall ChickenBlockerUpd.c -o ChickenBlockerUpd) || {
    echo "ERROR: could not build $UPDATER" >&2; exit 1; }
fi

# 1. Clear +i via a cap-endowed transient service running the raw-ioctl updater.
echo "Clearing +i on stuck legacy files via systemd-run + $UPDATER ..."
clear_list=()
for p in "${STUCK[@]}"; do
  if [ -e "$p" ] && lsattr "$p" 2>/dev/null | cut -c1-22 | grep -q 'i'; then
    clear_list+=("$p")
  else
    echo "  $p : already gone or not +i; skipping"
  fi
done

if [ "${#clear_list[@]}" -gt 0 ]; then
  if command -v systemd-run >/dev/null 2>&1; then
    unit="cb-stuck-clear-$$-$(date +%s%N)"
    systemd-run --quiet --wait --collect --pipe --unit="$unit" \
      -p User=root \
      -p CapabilityBoundingSet=CAP_LINUX_IMMUTABLE \
      -p AmbientCapabilities=CAP_LINUX_IMMUTABLE \
      "$UPDATER" clear "${clear_list[@]}" || {
        echo "WARN: updater clear via systemd-run failed (legacy BPF gate still attached?)." >&2
        echo "      Falling back to chattr -i via systemd-run for the non-apt-pin files." >&2
        # Fallback: chattr -i via systemd-run for files NOT denied by the legacy
        # chattr AppArmor profile (the apt pin IS denied; the apparmor.d files +
        # override are not). If this also fails, reboot (the cap-drop drop-ins
        # are already removed, so a fresh session regains the cap) then re-run.
        for p in "${clear_list[@]}"; do
          case "$p" in
            /etc/apt/preferences.d/*) echo "  SKIP chattr fallback on $p (AppArmor denies; needs reboot+chattr)" >&2 ;;
            *) systemd-run --quiet --wait --collect --pipe \
                 -p CapabilityBoundingSet=CAP_LINUX_IMMUTABLE \
                 -p AmbientCapabilities=CAP_LINUX_IMMUTABLE \
                 chattr -i "$p" 2>/dev/null && echo "  chattr -i ok: $p" || echo "  chattr -i FAILED: $p" >&2 ;;
          esac
        done
      }
  else
    echo "ERROR: systemd-run not available. Reboot (cap-drop drop-ins are gone, so a" >&2
    echo "       fresh session regains CAP_LINUX_IMMUTABLE), then run: sudo chattr -i ${clear_list[*]}" >&2
    exit 1
  fi
fi

# 2. Remove the files now that +i is cleared.
echo "Removing cleared legacy files ..."
for p in "${STUCK[@]}"; do
  if [ -e "$p" ]; then
    rm -f "$p" 2>/dev/null && echo "  removed $p" || echo "  WARN: could not rm $p (still +i? reboot then re-run)" >&2
  fi
done
# Remove the apparmor override dir if empty.
rmdir /etc/systemd/system/apparmor.service.d 2>/dev/null || true

# 3. Unload the legacy AppArmor profiles from the running kernel (best-effort).
#    They were never unloaded because the legacy override blocked `systemctl stop
#    apparmor` and the parser profile blocked reading the files. Read from the
#    repo source path (not /etc/apparmor.d, which the parser profile denies) to
#    unload by name. The new apparmor/install.sh reloads them with ChickenBlocker
#    deny paths via -r (replace), so this is optional — but cleaner.
echo "Unloading legacy AppArmor profiles from kernel (best-effort) ..."
for f in usr.bin.chattr usr.bin.apparmor_parser shell-bpf; do
  src="$REPO_ROOT/apparmor/$f"
  [ -f "$src" ] && apparmor_parser -R "$src" 2>/dev/null && echo "  unloaded $f" || true
done

# 4. Drop the stale `cockblock.target` still loaded in systemd's memory.
systemctl daemon-reload 2>/dev/null || true
systemctl reset-failed cockblock.target cockblock.service cockblock-bpf.service \
  cockblock-bpf-firstboot.service cockblock-update.service 2>/dev/null || true

echo
echo "Stuck legacy files cleared."
echo "Now finish the ChickenBlocker install:"
echo "  sudo bpf/install.sh"
echo "  sudo ./apparmor/install.sh"
echo "  sudo ./update.sh"
