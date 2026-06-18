#!/usr/bin/env bash
# Quiet the system for benchmarking: cpufreq -> performance, turbo / SMT /
# THP / KSM / ASLR / NMI watchdog off, noisy services stopped, page cache
# dropped. Snapshots prior state to /tmp/qwc-bench-state so 'restore' is a
# proper revert -- even if the bench dies before its EXIT trap fires, you
# can rerun 'sudo bench-prep.sh restore' to get the box back.
#
# Usage:
#   sudo ./scripts/bench-prep.sh apply
#   sudo ./scripts/bench-prep.sh restore
#   ./scripts/bench-prep.sh status
#
# Out of scope (need a reboot or risk locking the box):
#   isolcpus= / nohz_full= / mitigations= -- kernel cmdline, do in bootloader.
#   NetworkManager / wpa_supplicant       -- fine wired, lethal over ssh/wifi.
#   IRQ affinity rewrites                 -- system-specific.

set -euo pipefail

# Linux-only: every knob below lives under /sys, /proc, or systemd. The
# macOS / Apple Silicon equivalents (mdutil, purge, taskpolicy, launchctl)
# are a different tool entirely; bench-sweep.sh skips the prep call
# on non-Linux rather than trying to paper over the gap here.
if [ "$(uname -s)" != "Linux" ]; then
  echo "bench-prep.sh: Linux-only (got $(uname -s)). Skipping." >&2
  exit 0
fi

STATE=/tmp/qwc-bench-state

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
  exit 64
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (try: sudo $0 $*)" >&2
    exit 1
  fi
}

# Append a literal command to the restore script being built up in $STATE.
remember() { printf '%s\n' "$*" >> "$STATE"; }

# write_sys <path> <new>  -- snapshot current value, then write new.
# Silently skips paths that don't exist on this kernel/CPU.
write_sys() {
  local path="$1" newval="$2" oldval
  [ -e "$path" ] || return 0
  oldval="$(cat "$path" 2>/dev/null || true)"
  [ -n "$oldval" ] || return 0
  remember "echo $(printf %q "$oldval") > $(printf %q "$path") 2>/dev/null || true"
  echo "$newval" > "$path" 2>/dev/null || true
}

apply() {
  if [ -e "$STATE" ] && [ "${1:-}" != "force" ]; then
    echo "$STATE already exists. Run '$0 restore' first, or '$0 force-apply'." >&2
    exit 1
  fi
  : > "$STATE"
  chmod 600 "$STATE"
  remember "#!/usr/bin/env bash"
  remember "# Auto-generated revert script. Executed by bench-prep.sh restore."
  remember "set +e"

  # cpufreq governor -> performance, snapshot per-CPU.
  for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [ -e "$g" ] || continue
    oldgov="$(cat "$g")"
    remember "echo $(printf %q "$oldgov") > $(printf %q "$g") 2>/dev/null || true"
    echo performance > "$g" 2>/dev/null || true
  done

  # Turbo off (Intel and AMD paths; only one will exist on a given box).
  write_sys /sys/devices/system/cpu/intel_pstate/no_turbo 1
  write_sys /sys/devices/system/cpu/cpufreq/boost 0

  # SMT off.
  write_sys /sys/devices/system/cpu/smt/control off

  # Memory / kernel knobs.
  write_sys /sys/kernel/mm/transparent_hugepage/enabled never
  write_sys /sys/kernel/mm/ksm/run 0
  write_sys /proc/sys/kernel/randomize_va_space 0
  write_sys /proc/sys/kernel/nmi_watchdog 0

  # C-states: shallowest only. Restore re-enables all (acceptable
  # simplification -- per-state-per-CPU snapshotting is overkill here).
  if command -v cpupower >/dev/null; then
    cpupower idle-set -D 0 >/dev/null 2>&1 || true
    remember "cpupower idle-set -E >/dev/null 2>&1 || true"
  fi

  # Swap off only if it was actually on (so we don't 'swapon -a' on a
  # box that deliberately has no swap).
  if [ "$(wc -l < /proc/swaps)" -gt 1 ]; then
    swapoff -a || true
    remember "swapon -a 2>/dev/null || true"
  fi

  # Stop noisy services that are currently active. Service names cover
  # Arch / Debian / Fedora variants; missing units are silently skipped.
  local services=(
    cron cronie crond anacron atd
    snapd packagekit fwupd cups cups.socket
    bluetooth ModemManager
    tracker-miner-fs-3.service tracker-miner-rss-3.service
    tracker-extract-3.service
    baloo_file mlocate-updatedb.timer updatedb.timer plocate-updatedb.timer
    irqbalance
  )
  local svc
  for svc in "${services[@]}"; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
      systemctl stop "$svc" 2>/dev/null || true
      remember "systemctl start $(printf %q "$svc") 2>/dev/null || true"
    fi
  done

  # Drop page cache (no restore -- the cache will refill on its own).
  sync
  echo 3 > /proc/sys/vm/drop_caches

  # Make restore self-clean: the last thing the revert script does is
  # remove itself, so a successful 'restore' leaves no $STATE behind.
  remember "rm -f $(printf %q "$STATE")"

  echo "bench mode applied. state: $STATE"
}

restore() {
  if [ ! -e "$STATE" ]; then
    echo "no state file at $STATE; nothing to restore."
    return 0
  fi
  bash "$STATE"
  echo "bench mode reverted."
}

status() {
  if [ -e "$STATE" ]; then
    echo "bench mode: ACTIVE ($STATE)"
    exit 0
  else
    echo "bench mode: inactive"
    exit 1
  fi
}

case "${1:-}" in
  apply)        require_root "$@"; apply ;;
  force-apply)  require_root "$@"; rm -f "$STATE"; apply force ;;
  restore)      require_root "$@"; restore ;;
  status)       status ;;
  *)            usage ;;
esac
