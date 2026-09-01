#!/usr/bin/env python3
# Decode the Le Chat Mauve Eve's PLS100/82S100 fuse map into product terms.
#
# Input: docs/Chat_Mauve_eve_PLA.jed (Apple II Documentation Project, "ROM
# Images" — a Max Loader JEDEC dump of the N82S100N on the board).
#
# Layout (QF1928): 48 rows of 40 fuses — per PRODUCT TERM, 16 input pairs
# (true, complement; fuse 0 = intact = connected) then 8 OR fuses (0 =
# term feeds that output) — followed by 8 output-polarity fuses (0 =
# active-high; F6 and F7 read active-low). The convention is fixed by
# structure: it is the one that yields 48 clean terms, every one driving a
# small output set, with the short TEXT-side terms (`I0 & I4`,
# `/I0 & I4 & /I15`, `I4 & I9`) the 2026-09-01 research pass described.
#
# docs/chatmauve_plan.md § 3.5 carries the decoded table and the reading of
# it; this script is how to regenerate that table from the dump.

import re
import sys
from pathlib import Path

jed = Path(sys.argv[1] if len(sys.argv) > 1 else
           Path(__file__).parent.parent / "docs" / "Chat_Mauve_eve_PLA.jed")
d = jed.read_text(errors="replace")

fuses = {}
for m in re.finditer(r"L(\d+)\s+([01\s]+)\*", d):
    a = int(m.group(1))
    bits = re.sub(r"\s", "", m.group(2))
    for i, b in enumerate(bits):
        fuses[a + i] = int(b)
assert max(fuses) + 1 == 1928, "expected a full QF1928 PLS100 map"

rows = [[fuses[t * 40 + k] for k in range(40)] for t in range(48)]
pol = [fuses[1920 + k] for k in range(8)]
print("output polarity (1 = active-low):", pol)

for t in range(48):
    r = rows[t]
    lits = []
    for i in range(16):
        tr, cm = r[i * 2], r[i * 2 + 1]
        if tr == 0 and cm == 0:
            lits = None            # contradictory — term never true
            break
        if tr == 0:
            lits.append(f"I{i}")
        elif cm == 0:
            lits.append(f"/I{i}")
    outs = [f"F{f}" for f in range(8) if r[32 + f] == 0]
    if lits is None:
        print(f"T{t:2d}: (never true)")
        continue
    print(f"T{t:2d}: {' & '.join(lits) if lits else '1':70s} -> {','.join(outs)}")
