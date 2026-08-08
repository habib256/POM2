#!/usr/bin/env bash
# =============================================================================
#  pi_tuning.sh — system-side tuning for running POM2 on a Raspberry Pi.
#
#  The build recipe (build_native_pi.sh --pgo) makes the emulator faster. This
#  script removes the things the SYSTEM does that make it stutter — which is a
#  different problem: a Pi that averages 3× realtime can still hitch audibly
#  every few seconds. In rough order of how much each one is worth:
#
#   1. CPU governor → `performance`. Raspberry Pi OS ships `ondemand`; the
#      frequency ramps it does produce exactly the periodic "it goes in jerks"
#      stutter, because the emulator's load is bursty at 60 Hz and the governor
#      keeps deciding the core is idle.
#   2. Hardware IRQs pinned to core 0 (`irqaffinity=0` on the kernel cmdline).
#      The emulation thread then no longer shares its core with USB/Ethernet
#      interrupt handling.
#   3. Swap off. On an SD card, one swap-in in the middle of a frame is a
#      guaranteed audio dropout.
#
#  What this script deliberately does NOT do: install a kiosk session, remove
#  the desktop, or touch the audio stack. POM2 has kiosk mode built in (F10,
#  or `--kiosk` on the command line) and its own audio device selection — see
#  README.md. Tuning and deployment are kept separate on purpose, so you can
#  apply this on a normal desktop Pi you also use for other things.
#
#  Usage (ON the Pi, as root):
#      sudo packaging/raspberry/pi_tuning.sh
#      sudo packaging/raspberry/pi_tuning.sh --no-irq-pin   # skip the cmdline edit
#      sudo packaging/raspberry/pi_tuning.sh --uninstall
#
#  Idempotent: running it twice changes nothing the second time. The cmdline
#  edit is the only change that needs a reboot, and the script says so.
#
#  (c) 2026 VERHILLE Arnaud — POM2.
# =============================================================================
set -euo pipefail

UNINSTALL=0
IRQ_PIN=1
for a in "$@"; do
    case "$a" in
        --uninstall)  UNINSTALL=1 ;;
        --no-irq-pin) IRQ_PIN=0 ;;
        *) echo "unknown option: $a  (--uninstall, --no-irq-pin)"; exit 1 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "ERROR: this script needs root (sudo)"; exit 1; }
log() { echo "[pi_tuning] $*"; }

# Raspberry Pi OS moved the boot partition from /boot to /boot/firmware in
# bookworm. Pick whichever actually holds cmdline.txt; if neither does we are
# not on a Pi image and the cmdline step is skipped rather than guessed at.
BOOTDIR=""
for d in /boot/firmware /boot; do
    [ -f "$d/cmdline.txt" ] && { BOOTDIR="$d"; break; }
done

MARK="# added by POM2 pi_tuning.sh"

# ── Uninstall ───────────────────────────────────────────────────────────────
if [ "$UNINSTALL" = "1" ]; then
    systemctl disable --now pom2-perf.service 2>/dev/null || true
    rm -f /etc/systemd/system/pom2-perf.service
    systemctl daemon-reload
    if [ -n "$BOOTDIR" ] && [ -f "$BOOTDIR/cmdline.txt.pom2-bak" ]; then
        mv -f "$BOOTDIR/cmdline.txt.pom2-bak" "$BOOTDIR/cmdline.txt"
        log "restored $BOOTDIR/cmdline.txt — reboot to apply"
    fi
    log "swap left as it is (re-enable with: systemctl enable --now dphys-swapfile)"
    log "uninstalled."
    exit 0
fi

# ── 1. CPU governor ─────────────────────────────────────────────────────────
# Done as a systemd unit rather than a one-shot write: the governor resets on
# every boot, and on some kernels cpufreq comes up after the first userspace
# writes. `|| true` per policy so a core without cpufreq doesn't fail the unit.
cat > /etc/systemd/system/pom2-perf.service <<EOF
[Unit]
Description=POM2 — pin the CPU governor to performance
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo performance > "\$g" || true; done'

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now pom2-perf.service
log "governor: $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || echo '?')"

# ── 2. IRQ affinity ─────────────────────────────────────────────────────────
if [ "$IRQ_PIN" = "1" ] && [ -n "$BOOTDIR" ]; then
    CMDLINE="$BOOTDIR/cmdline.txt"
    if grep -q "irqaffinity=" "$CMDLINE"; then
        log "cmdline already carries irqaffinity= — left alone"
    else
        # cmdline.txt must stay ONE line: appending a newline makes the kernel
        # ignore everything after it, and the symptom (no root filesystem) is
        # spectacular. Hence the tr and the backup.
        cp -n "$CMDLINE" "$CMDLINE.pom2-bak"
        printf '%s irqaffinity=0\n' "$(tr -d '\n' < "$CMDLINE")" > "$CMDLINE.new"
        mv -f "$CMDLINE.new" "$CMDLINE"
        log "added irqaffinity=0 to $CMDLINE (backup: $CMDLINE.pom2-bak) — REBOOT to apply"
    fi
elif [ "$IRQ_PIN" = "1" ]; then
    log "no cmdline.txt found (/boot/firmware or /boot) — IRQ pinning skipped"
fi

# ── 3. Swap ─────────────────────────────────────────────────────────────────
if [ -x /usr/sbin/dphys-swapfile ]; then
    dphys-swapfile swapoff 2>/dev/null || true
    systemctl disable --now dphys-swapfile 2>/dev/null || true
    log "swap disabled (dphys-swapfile)"
else
    log "dphys-swapfile absent — swap left as configured"
fi

log "done. Measure the result with:  pom2_bench --disk <image> --frames 900"
log "(see docs/PERFORMANCE.md; ${MARK#\# } is reversible with --uninstall)"
