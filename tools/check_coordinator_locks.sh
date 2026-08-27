#!/usr/bin/env bash
# POM2 — coordinator lock-scope scan.
#
# Every pom2::*Coordinator capture/apply call takes the machine lock itself.
# `stateMutex` is NON-RECURSIVE, so such a call placed inside an existing
# lock scope hangs the UI thread and the emulator with it — and no test
# catches it, because nothing drives the ImGui panels (TODO § [Arch]).
#
# That is not hypothetical: wiring PrinterCoordinator into
# renderFujiNetPanelWindow put a captureHost() inside a
# lock_guard(stateMutex()) scope, the full suite stayed green, and the panel
# would have hung on first open.
#
# Run after touching any MainWindow*.cpp coordinator call site:
#     tools/check_coordinator_locks.sh
#
# Exit 0 = clean, 1 = at least one call site sits inside an open lock scope.
#
# BOTH lock spellings must be checked. The first version of this scan looked
# only for `lockState()` and therefore missed the bug above, which uses the
# bare `stateMutex()`.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

python3 - "$@" <<'PY'
import glob
import re
import sys

# Calls that resolve a card under the machine lock. Add new coordinators here.
COORD = (
    'mouseCoordinator_->',
    'printerCoordinator_->',
    'audioCoordinator_->',
    'devicePanelCoordinator_->',
    'storageCoordinator_->',
    'slotConfigurationCoordinator_->',
    'slotProvisioningCoordinator_->',
    'slotRebuildCoordinator_->',
    'debugCoordinator_->',
)

# Members that deliberately take NO lock, so they are legal inside one.
# Keep this list short and justify every entry.
LOCK_FREE = (
    'resetFeedCursor',   # plain member reset, no machine access
)

FUNC_START = re.compile(r'^[A-Za-z_][\w:<>,&*\s]*::\w+\(')

def enclosing_function(lines, idx):
    for j in range(idx, -1, -1):
        if FUNC_START.match(lines[j]):
            return j
    return 0

def lock_still_open(lines, fn_start, call_idx):
    """Nearest preceding lock, then check whether its block has closed."""
    lock = None
    for j in range(call_idx - 1, fn_start - 1, -1):
        code = lines[j].split('//')[0]
        if 'lockState()' in code or 'stateMutex()' in code:
            lock = j
            break
    if lock is None:
        return None
    depth = 0
    for j in range(lock, call_idx):
        code = lines[j].split('//')[0]
        depth += code.count('{') - code.count('}')
        if j > lock and depth < 0:
            return None          # scope closed before the call
    return lock + 1

bad = 0
checked = 0
for path in sorted(glob.glob('src/MainWindow*.cpp')):
    lines = open(path, encoding='utf-8').read().split('\n')
    for i, line in enumerate(lines):
        if not any(c in line for c in COORD):
            continue
        if any(x in line for x in LOCK_FREE):
            continue
        checked += 1
        held = lock_still_open(lines, enclosing_function(lines, i), i)
        if held is not None:
            bad += 1
            print(f"DEADLOCK RISK  {path}:{i + 1}")
            print(f"    call : {line.strip()[:96]}")
            print(f"    lock : {path}:{held} is still open here")

if bad:
    print(f"\n{bad} call site(s) inside an open lock scope, of {checked} checked.")
    print("Move the call after the lock scope closes, or take the value from a")
    print("snapshot captured before the lock was acquired.")
    sys.exit(1)

print(f"clean — {checked} coordinator call site(s), none inside a lock scope")
PY
