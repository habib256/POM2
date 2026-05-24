POM2 — where to put your ROMs
=============================

POM2 ships WITHOUT Apple II ROM images: they are Apple Computer's
copyrighted firmware and cannot legally be redistributed. You must
supply your own dumps (extracted from real hardware you own, or
obtained from an archive you are entitled to use).

Drop the ROM files into THIS directory (the `roms/` folder next to the
POM2 binary). POM2 also looks in your per-user data dir, so on a
system-wide install you can instead place them in:

    ~/.local/share/POM2/roms/

Minimum to boot an Apple ][+ (the default profile):

    apple2.rom        12 KB ($D000-$FFFF) or 16 KB ($C000-$FFFF)
    apple2_char.rom    2 KB  character generator (optional; a built-in
                             5x7 ASCII font is used when absent)

Other profiles look for, in probe order (see README.md for the full
table):

    apple2o.rom / apple2p.rom      Apple ][ (1977) / ][+ (1979)
    apple2e.rom                    Apple //e (16 or 32 KB)
    apple2c-32Kv0.rom              Apple //c
    apple2cp.rom                   Apple //c+

Disk II (5.25") boot:

    disk2.rom          256 B   16-sector P5A boot PROM
    diskii_p6.rom      256 B   P6 LSS sequencer (required for .woz)

See the bundled README.md for the complete ROM list and accepted sizes.
