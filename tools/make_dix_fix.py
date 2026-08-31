#!/usr/bin/env python3
# POM2 Apple II Emulator
# Copyright (C) 2026 VERHILLE Arnaud
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""Build DIX-fix.po — DIX with the "RETURN at the menu" off-by-one fixed.

THE BUG (DIX's own, faithfully reproduced by POM2 — see
`docs/test_corpus.md` § "DIX menu: RETURN before any arrow key wedges" and
`tests/dix_return_crash_probe.cpp`).

DIX keeps the highlighted menu entry in `CurrentChoice = $DFFF` and
initialises it to 0 at cold boot (`loader.a:119-121`). Entry 0 is the
"USE ARROWS TO SELECT DEMO" prompt, so only an arrow key ever gives
`CurrentChoice` a valid 1..16. But the launcher indexes a **16-entry,
one-based** jump table (`loader.a:117-153`):

    $D02C  AE FF DF   LDX CurrentChoice
    $D02F  CA         DEX                  ; 0 -> $FF   <-- underflow
    $D030  BD 76 D0   LDA DemosL,X         ; $D175 = $E1
    $D036  BD 86 D0   LDA DemosH,X         ; $D185 = $17
    $D03C  20 .. ..   JSR $17E1            ; garbage -> BRK storm

so RETURN (or SPACE) pressed before any arrow key jumps into unwritten
RAM: picture frozen, Mockingboard music still playing.

THE FIX. DIX already ships the behaviour we want as **menu entry 16**,
"ALL DEMOS IN AUTOMATIC MODE" (`AUTOMODE`, `loader.a:132-148`) — it sets
`ExpoMode = 1` and chains all fifteen parts forever. Table entry 15
(0-based) is `$D042` = `AUTOMODE`. So an index of 0 is redirected to it:
RETURN on the fresh menu now plays the whole anthology instead of dying.

Reaching that costs one conditional, and the loader image ($D000-$DDFF,
blocks 1-7) is packed solid — but the boot block reads **8** blocks
(`boot_unidisk.a` `SP_BLOCKS2READ = 8`), so block 8 lands at
$DE00-$DFFF, and its tail $DF56-$DFFE is 169 zero bytes of loaded,
resident RAM. The 9-byte stub goes there.

    $D02C  20 60 DF   JSR PICKDEMO        ; was LDX $DFFF
    $D02F  EA         NOP                 ; was DEX

    PICKDEMO = $DF60
    $DF60  AE FF DF   LDX CurrentChoice
    $DF63  CA         DEX
    $DF64  10 02      BPL +
    $DF66  A2 0F      LDX #15             ; AUTOMODE
    $DF68  60       + RTS

`JSR`/`RTS` leave X untouched, and `LDA DemosL,X` at $D030 does not care
about the incoming flags, so the fall-through path is bit-identical to
the original. Two edits, 13 bytes; every other byte of the 800 KB image
is copied verbatim.

Usage:  tools/make_dix_fix.py [in.po] [out.po]        (defaults below)
        tools/make_dix_fix.py --verify out.po         (check a built one)
"""

import sys

DEFAULT_IN  = "disks_3.5/DIX.po"
DEFAULT_OUT = "disks_3.5/DIX-fix.po"

IMAGE_SIZE = 800 * 1024
BLOCK      = 512

# --- site 1: the launcher's index fetch, block 1 = $D000 ------------------
SITE_DISPATCH      = 1 * BLOCK + 0x2C          # $D02C
ORIG_DISPATCH      = bytes([0xAE, 0xFF, 0xDF,  # LDX $DFFF
                            0xCA])             # DEX
PATCH_DISPATCH     = bytes([0x20, 0x60, 0xDF,  # JSR $DF60
                            0xEA])             # NOP

# --- site 2: the stub, block 8 = $DE00, offset $160 -> $DF60 --------------
SITE_STUB          = 8 * BLOCK + 0x160         # $DF60
ORIG_STUB          = bytes(9)                  # must be virgin padding
PATCH_STUB         = bytes([0xAE, 0xFF, 0xDF,  # LDX $DFFF
                            0xCA,              # DEX
                            0x10, 0x02,        # BPL +2
                            0xA2, 0x0F,        # LDX #15   (AUTOMODE)
                            0x60])             # RTS

# --- invariants the patch leans on (checked, never written) --------------
#     DemosH[15] . DemosL[15] must be $D042, the AUTOMODE entry point,
#     and $D042 must still be `LDA #1 / STA ExpoMode`.
SITE_DEMOSL15      = 1 * BLOCK + 0x76 + 15
SITE_DEMOSH15      = 1 * BLOCK + 0x86 + 15
SITE_AUTOMODE      = 1 * BLOCK + 0x42
ORIG_AUTOMODE      = bytes([0xA9, 0x01, 0x85, 0x04])   # LDA #1 / STA $04


def fail(msg):
    sys.stderr.write("make_dix_fix: %s\n" % msg)
    sys.exit(1)


def check_invariants(img):
    """Assert the jump table still points entry 16 at AUTOMODE."""
    lo, hi = img[SITE_DEMOSL15], img[SITE_DEMOSH15]
    if (hi << 8 | lo) != 0xD042:
        fail("jump-table entry 16 is $%04X, expected $D042 (AUTOMODE); "
             "this is not the DIX image this patch was written for"
             % (hi << 8 | lo))
    if img[SITE_AUTOMODE:SITE_AUTOMODE + 4] != ORIG_AUTOMODE:
        fail("$D042 is not `LDA #1 / STA ExpoMode`; AUTOMODE has moved")


def verify(path):
    img = bytearray(open(path, "rb").read())
    if len(img) != IMAGE_SIZE:
        fail("%s: %d bytes, expected %d" % (path, len(img), IMAGE_SIZE))
    check_invariants(img)
    ok = True
    for name, off, want in (("dispatch", SITE_DISPATCH, PATCH_DISPATCH),
                            ("stub",     SITE_STUB,     PATCH_STUB)):
        got = bytes(img[off:off + len(want)])
        state = "OK" if got == want else "MISSING"
        if got != want:
            ok = False
        print("  %-8s @ %-8s %s   %s" %
              (name, "$%04X" % (0xD000 + off - BLOCK), state, got.hex(" ")))
    print("%s: %s" % (path, "patched" if ok else "NOT patched"))
    return 0 if ok else 1


def main(argv):
    if argv and argv[0] == "--verify":
        return verify(argv[1] if len(argv) > 1 else DEFAULT_OUT)

    src = argv[0] if len(argv) > 0 else DEFAULT_IN
    dst = argv[1] if len(argv) > 1 else DEFAULT_OUT

    try:
        img = bytearray(open(src, "rb").read())
    except OSError as e:
        fail("cannot read %s: %s" % (src, e))
    if len(img) != IMAGE_SIZE:
        fail("%s: %d bytes, expected a raw 800 KB .po (%d)"
             % (src, len(img), IMAGE_SIZE))

    check_invariants(img)

    # Refuse to patch anything that is not the pristine original: an
    # already-patched or differently-built image must not be mangled.
    for name, off, want in (("dispatch", SITE_DISPATCH, ORIG_DISPATCH),
                            ("stub",     SITE_STUB,     ORIG_STUB)):
        got = bytes(img[off:off + len(want)])
        if got != want:
            fail("%s site @ $%04X holds %s, expected %s — refusing to patch"
                 % (name, 0xD000 + off - BLOCK, got.hex(" "), want.hex(" ")))

    img[SITE_DISPATCH:SITE_DISPATCH + len(PATCH_DISPATCH)] = PATCH_DISPATCH
    img[SITE_STUB:SITE_STUB + len(PATCH_STUB)]             = PATCH_STUB

    with open(dst, "wb") as f:
        f.write(img)

    orig = bytearray(open(src, "rb").read())
    diff = [i for i in range(IMAGE_SIZE) if orig[i] != img[i]]
    print("wrote %s (%d bytes, %d bytes differ from %s)"
          % (dst, len(img), len(diff), src))
    print("  $D02C  JSR $DF60 / NOP      (was LDX $DFFF / DEX)")
    print("  $DF60  LDX $DFFF / DEX / BPL +2 / LDX #15 / RTS")
    print("  RETURN or SPACE with no selection -> entry 16, "
          "ALL DEMOS IN AUTOMATIC MODE")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
