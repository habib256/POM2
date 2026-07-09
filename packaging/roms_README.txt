POM2 — Apple ROMs go here
==========================

POM2 does NOT ship Apple's ROMs: they are copyrighted by Apple and are not
redistributable. Drop your own dumps into this directory (the one containing
this file) and POM2 will pick them up on the next launch.

Expected files (only the ones for the profiles you use are needed)
------------------------------------------------------------------
  Main system ROMs
    apple2.rom            generic fallback (12 KB $D000-$FFFF or 16 KB $C000-)
    apple2o.rom           Apple ][ Original (1977)
    apple2p.rom           Apple ][+ (1979)
    apple2e.rom           Apple //e Enhanced (1985) + PAL profile
    apple2e_unenh.rom     Apple //e Unenhanced (1983)
    apple2c-32Kv0.rom     Apple //c (1984)   (or apple2c-16K.rom)
    apple2cp.rom          Apple //c Plus (1988)

  Character ROMs (optional — a built-in 5x7 font is used when absent)
    apple2_char.rom       Apple ][ / ][+ character generator
    apple2e_char.rom      Apple //e character generator

  Peripheral ROMs (optional — cards fall back to stubs when absent)
    disk2.rom             Disk II 16-sector boot PROM (slot 6 auto-boot)
    disk2_13.rom          Disk II 13-sector boot PROM
    diskii_p6.rom         Disk II P6 LSS PROM
    mouse_341-0270-c.bin  AppleMouse II card firmware
    cffa20eec02.bin       CFFA 2.0 card firmware
    grappler_plus.bin     Orange Micro Grappler+ 4 KB EPROM

Where to find them
------------------
POM2 also probes a `roms/` folder next to the executable and, on Linux,
~/.local/share/POM2/roms/. A ROM's exact byte layout must match the machine
you select; if only apple2.rom is present, POM2 warns it may not match.

Notes
-----
* A 12 KB main ROM maps to $D000-$FFFF; a 16 KB one to $C000-$FFFF.
* The floppy_samples/ subfolder here (mechanical drive sounds) IS bundled and
  is not a ROM — leave it in place.
