# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

POM2's **FPGA companion** to the C++ emulator (`/home/gistarcade/src/POM2`,
see `../CLAUDE.md`). Real silicon target — synthesizes onto MiSTer
(Cyclone V on a DE10-Nano).

## What lives here

```
fpga/
├── Apple-II_MiSTer/          # vendored upstream core (own .git)
│   ├── Apple-II.sv           # MiSTer wrapper module `emu` — OSD CONF_STR + HPS_BUS
│   ├── Apple-II.qsf / .qpf   # Quartus project files
│   ├── files.qip             # source list (HDL files registered with Quartus)
│   ├── rtl/                  # core HDL (mostly VHDL, some SV)
│   ├── sys/                  # MiSTer framework (sys_top.v is the chip-level top)
│   ├── Palettes/             # NTSC / IIgs / AppleWin / custom .a2p files
│   └── output_files/         # Quartus build outputs (gitignored)
└── Apple-II_MiSTer.rbf       # deployable bitstream — copied to MiSTer SD
```

`Apple-II_MiSTer/` is a clone of
`https://github.com/MiSTer-devel/Apple-II_MiSTer.git` (`master` branch).
Its `.git` is independent of the parent POM2 repo — `git status` from the
POM2 root sees the whole `fpga/` directory as untracked, while `git
status` inside `Apple-II_MiSTer/` tracks against the upstream remote.
**Treat upstream files as read-only**: any local edit there should be
intentional and minimal, since rebasing onto upstream is the normal
update path. Today the only local diff is a Quartus version-stamp bump
in `Apple-II.qsf` and an auto-generated `sys/pll_q25.qip`.

## Build & deploy

Toolchain: **Intel Quartus Prime Lite 25.1std.0** (the upstream project
file claims 17.0.2 — the version-stamp diff in `Apple-II.qsf` is the only
needed change to open it under the newer IDE).

```bash
# GUI flow
quartus Apple-II_MiSTer/Apple-II.qpf   # then Processing → Start Compilation

# CLI flow (full compile → bitstream)
cd Apple-II_MiSTer
quartus_sh --flow compile Apple-II

# After a successful compile, copy the bitstream up so it deploys
cp Apple-II_MiSTer/output_files/Apple-II.rbf Apple-II_MiSTer.rbf
```

The current `fpga/Apple-II_MiSTer.rbf` (3.5 MB, dated 2026-05-08) was
copied from a prior compile — running only **Analysis & Synthesis**
(which is what's reflected in `output_files/Apple-II.map.summary`) does
**not** regenerate the .rbf. Always run the full flow before bumping
the deployable artifact.

Deploy: drop `Apple-II_MiSTer.rbf` into `/_Computer/` (or rename to
`Apple-II_<date>.rbf`) on the MiSTer SD card. ROMs go in
`/games/Apple-II/` on the same SD; nothing in this repo bundles them.

## Architecture (big picture)

Two separate concerns are layered:

1. **MiSTer integration shell** (`Apple-II.sv` + `sys/`).
   `sys_top` is the chip top (set by `Apple-II.qsf` via `set_global_assignment
   -name TOP_LEVEL_ENTITY sys_top`). It instantiates `emu` (the core),
   feeds it `CLK_50M`, and routes `HPS_BUS` to the ARM-side Linux running
   the MiSTer menu. `emu` declares the OSD layout in the `CONF_STR`
   parameter (line ~213 of `Apple-II.sv`); each character there maps to
   a bit of the 32-bit `status` word that the core reads to know what
   the user picked. Disk / HDV mounts come in over the `sd_*` and
   `img_mounted` interfaces. Custom palette uploads come in as
   `ioctl_download` files.

2. **Apple II core** (`rtl/apple2_top.vhd` + friends). Pure-hardware
   reconstruction: `apple2.vhd` is the address-decode top (Wozniak's
   architecture), `timing_generator.vhd` runs the 14.31818 MHz dot
   clock and derives the line counters that drive both the row
   interleave and DRAM refresh, `video_generator.vhd` shifts pixels
   for text / lo-res / hi-res, `vga_controller.vhd` interpolates the
   composite-style color signal into a 640×480 VGA stream, and
   `keyboard.vhd` adapts the PS/2 stream from `hps_io` into the Apple's
   key latch + strobe at $C000. CPU is selectable between **T65**
   (`rtl/t65/`, 6502) and **R65Cx2** (`rtl/R65Cx2.vhd`, 65C02) via the
   `cpu_type` port — the OSD bit is `P1O5` in the CONF_STR.

Slot map (driven by the OSD's slot-select bits, see `Apple-II.sv` near
`mb_4_inslot` / `saturn_5_inslot`):

| Slot | Card                                       | RTL                                |
|------|--------------------------------------------|------------------------------------|
| 0    | Language Card                              | `ramcard.v`                        |
| 1    | ProDOS-compatible clock card               | `clock_card.v`                     |
| 2    | Super Serial Card                          | `rtl/ssc/super_serial_card.v`      |
| 3    | 80-col + 64K aux RAM (//e)                 | inside `apple2_top.vhd`            |
| 4    | Mockingboard (2× AY-3-8913) **or** Mouse   | `rtl/mockingboard/`, `rtl/mouse/`  |
| 5    | Saturn 128K **or** Mouse **or** Mockingbd. | `ramcard.v` / `rtl/mouse/`         |
| 6    | Disk II                                    | `rtl/disk_ii.vhd` + `drive_ii.vhd` |
| 7    | HDD controller                             | `rtl/hdd.vhd`                      |

Disk pipeline: `hps_io` mounts the .nib/.dsk/.do/.po image →
`floppy_track.sv` streams one track at a time into `dpram.vhd` → the
`disk_ii` controller reads nibbles back to the CPU. **Only the .nib
path supports writeback** (saves persisted to disk); the other formats
are read-only because the core would have to re-interleave on the fly.
HDV is 32 MB ProDOS partition images only — 2MG works after stripping
the 64-byte header (`dd bs=64 skip=1`).

## Gotchas

- **No tests / lint here.** The upstream repo doesn't ship a simulation
  harness; verification is "compile and try on hardware." Behavioural
  cross-checks against the C++ emulator are the practical sanity check.
- **`build_id.v` regenerates on every compile** (timestamp Verilog from
  `sys/build_id.tcl`) — never hand-edit, never check in.
- **Quartus version drift.** `Apple-II.qpf` still says `QUARTUS_VERSION
  = "17.0"`; the `LAST_QUARTUS_VERSION` in the .qsf is the live one.
  Don't "fix" the .qpf — opening under newer Quartus is fine.
- **`sys/pll_q25.qip` is auto-generated** by Quartus 25's PLL IP regen.
  It's currently untracked; safe to commit if you want reproducibility,
  but it'll churn whenever the IDE version moves.
- **The MiSTer framework (`sys/`) is not POM2 code.** Bug reports about
  scaler / scanlines / HDMI / OSD belong upstream
  (`MiSTer-devel/Main_MiSTer`), not against this repo. Keep local edits
  to `Apple-II.sv` and `rtl/`.
- **Status-bit conflicts.** The CONF_STR uses single-letter slots
  (`O5`, `OJK`, `OOP`, …). When adding an option, scan the existing
  string for a free letter — Quartus won't warn about a clash, the OSD
  just silently mis-routes the bit.
- **CLK_50M, CLK_VIDEO, clk_sys** all come from `pll`. The Apple II
  core wants 14.31818 MHz; that's `clk_sys`. `CLK_VIDEO` is the pixel
  clock for the VGA/HDMI pipeline. Don't reuse one for the other.
