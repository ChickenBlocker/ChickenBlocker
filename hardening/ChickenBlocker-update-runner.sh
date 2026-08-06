#!/bin/bash
# ChickenBlocker-update-runner.sh — the cap-endowed update worker.
#
# Runs as the ChickenBlocker-update systemd SYSTEM service (Type=oneshot), which has
# CAP_LINUX_IMMUTABLE in its bounding set (system services keep it even after the
# user-session cap-drop). It reads a manifest that update.sh writes to
# /run/ChickenBlocker-update-manifest, then for each protected file runs
#   ChickenBlockUpd update <dst> <src>   (atomic: clear +i -> copy -> re-+i)
# and reloads AppArmor (and BPF if requested). The user invokes it indirectly via
# `sudo ./update.sh` -> `systemctl start ChickenBlocker-update`; the user's own shell
# lacks CAP_LINUX_IMMUTABLE (after the cap-drop) so they CANNOT run ChickenBlockUpd
# themselves to clear +i — only this service can.
#
# Actor = a /tmp COPY of the installed updater (basename "ChickenBlockUpd" so the
# comm gate still matches; not +i so it runs; root service gives it the cap). Using
# the copy (not the installed binary) avoids ETXTBSY when updating the updater
# ITSELF (the installed binary would be the running actor otherwise).
set -euo pipefail

UPDATER_INST=/opt/ChickenBlocker/bpf/ChickenBlockUpd
MANIFEST=/run/ChickenBlocker-update-manifest

# Update freeze window: [22:00, 08:00). Refuse to refresh ANY protected file
# (incl. this runner itself) in this window, so `make update` can't be used to
# dodge the night block ([00:00, 08:00)). Self-protecting: this runner is the
# only actor that can refresh protected files (and itself), and it refuses to
# run in the window — so the gate can't be removed during the window either.
_cb_hour="$(date +%H)"
if [ "$_cb_hour" -ge 22 ] || [ "$_cb_hour" -lt 8 ]; then
  echo "ChickenBlocker-update: refused (update freeze window 22:00-08:00)" >&2
  rm -f "$MANIFEST" 2>/dev/null || true
  exit 1
fi

# Re-exec from a TEMP COPY of ourselves. The update loop below refreshes the
# INSTALLED runner at this path IN PLACE (it is in the PROTECTED list and so
# updates itself). bash reads this script as it runs; an in-place rewrite that
# changes the file length shifts bash's read offset into the middle of a token
# -> "syntax error near unexpected token" and the runner dies (exit 2) AFTER
# doing the updates. Running from a temp copy (a different inode) makes the
# loop's rewrite of the installed path a no-op for us. Mirrors the /tmp-copy
# trick used below for the updater binary (ETXTBSY). Guarded vs infinite loop.
if [ -z "${CHICKENBLOCKER_RUNNER_REEXEC:-}" ]; then
  _cb_self="$(mktemp)"
  cp "$0" "$_cb_self"
  chmod 0755 "$_cb_self"
  CHICKENBLOCKER_RUNNER_REEXEC=1 CHICKENBLOCKER_RUNNER_TMPCOPY="$_cb_self" exec "$_cb_self"
fi
# Re-execed: running from the temp copy now. Unlink it so it doesn't leak; bash
# keeps the open fd and the inode survives until we exit (Linux unlinked-but-open).
[ -n "${CHICKENBLOCKER_RUNNER_TMPCOPY:-}" ] && rm -f "$CHICKENBLOCKER_RUNNER_TMPCOPY" 2>/dev/null || true

if [ ! -f "$MANIFEST" ]; then
  echo "ChickenBlocker-update: no manifest at $MANIFEST" >&2
  exit 1
fi
if [ ! -x "$UPDATER_INST" ]; then
  echo "ChickenBlocker-update: $UPDATER_INST missing" >&2
  exit 1
fi

# /tmp copy of the updater as the actor (see header).
TMPD="$(mktemp -d)"
cp "$UPDATER_INST" "$TMPD/ChickenBlockUpd"
UPDATER="$TMPD/ChickenBlockUpd"
trap 'rm -rf "$TMPD"' EXIT

REPO=""
RELOAD_BPF=0
APT_PIN=0
ENTRIES=()

while IFS= read -r line; do
  [ -z "$line" ] && continue
  case "$line" in
    REPO=*)       REPO="${line#REPO=}" ;;
    RELOAD_BPF=*) RELOAD_BPF="${line#RELOAD_BPF=}" ;;
    APT_PIN=*)    APT_PIN="${line#APT_PIN=}" ;;
    /*)           ENTRIES+=("$line") ;;
    *)            echo "ChickenBlocker-update: bad manifest line '$line'" >&2 ;;
  esac
done < "$MANIFEST"

if [ -z "$REPO" ]; then
  echo "ChickenBlocker-update: manifest missing REPO=" >&2
  exit 1
fi

echo "=== ChickenBlocker-update: refreshing protected files (cap-endowed) ==="
for entry in "${ENTRIES[@]}"; do
  dst="${entry%%|*}"; src="${entry#*|}"
  if [ ! -e "$src" ]; then
    echo "SKIP $dst: source $src missing" >&2
    continue
  fi
  "$UPDATER" update "$dst" "$src"
done

echo
echo "=== ChickenBlocker-update: regenerating apt browser-install block ==="
if [ "$APT_PIN" = "1" ]; then
  PREF_FILE=/etc/apt/preferences.d/99-ChickenBlocker-no-browsers
  GEN="$REPO/apparmor/gen-browser-pin.sh"
  if [ -x "$GEN" ]; then
    mkdir -p "$(dirname "$PREF_FILE")"
    # We are cap-endowed (system service) so we can clear +i via the updater.
    if [ -f "$PREF_FILE" ] && lsattr "$PREF_FILE" 2>/dev/null | cut -c1-22 | grep -q 'i'; then
      "$UPDATER" clear "$PREF_FILE"
    fi
    "$GEN" > "$PREF_FILE"
    chmod 0644 "$PREF_FILE"
    # Re-+i. Setting +i is ungated by the BPF program.
    "$UPDATER" set "$PREF_FILE"
    echo "regenerated $PREF_FILE"
  else
    echo "SKIP apt pin: $GEN missing" >&2
  fi
else
  echo "(APT_PIN not requested; leaving apt pin as-is)"
fi

echo
echo "=== ChickenBlocker-update: reloading AppArmor profiles (from $REPO) ==="
for f in usr.bin.chattr usr.bin.apparmor_parser shell-bpf; do
  if apparmor_parser -Q "$REPO/apparmor/$f" >/dev/null 2>&1; then
    apparmor_parser -r "$REPO/apparmor/$f" && echo "reloaded $f" || echo "WARN: reload $f failed" >&2
  else
    echo "SKIP $f (syntax check failed)"
  fi
done

if [ "$RELOAD_BPF" = "1" ]; then
  echo
  echo "=== ChickenBlocker-update: reloading BPF live (uses loophole #2) ==="
  for pin in /sys/fs/bpf/ChickenBlocker_lsm /sys/fs/bpf/ChickenBlocker_setflags; do
    [ -e "$pin" ] && rm -f "$pin" && echo "detached $pin"
  done
  /opt/ChickenBlocker/bpf/ChickenBlocker_loader /opt/ChickenBlocker/bpf/ChickenBlocker_lsm.bpf.o
else
  echo
  echo "=== ChickenBlocker-update: BPF .o updated on disk; programs keep running (next boot) ==="
fi

rm -f "$MANIFEST"
echo "ChickenBlocker-update: done."
