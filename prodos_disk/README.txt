prodos_disk/ — host-folder mount for POM2's slot 5
====================================================

Drop ordinary files (e.g. STARTUP.BAS, HELLO.TXT, SHAPE.BIN) into this
directory. POM2 will synthesise a read-only ProDOS volume named /HOST/
on demand and expose it through the slot-5 ProDOS hard-disk card.

How to use:
  1. Drop / copy / symlink a few files into prodos_disk/.
  2. Launch POM2.
  3. In the "HDV (slot 5)" panel, click the "[host folder] prodos_disk/"
     entry — POM2 builds the volume and mounts it into the card.
  4. Boot ProDOS from another medium (a Disk II floppy in slot 6, or a
     bootable .hdv from the hdv/ directory).
  5. Once ProDOS is up, /HOST/ appears as a second drive. List it with
     CAT,S5,D1 (or via the Apple II's file manager of choice).

Limits (MVP):
  - Flat directory: sub-folders are ignored.
  - 51 files maximum (ProDOS volume directory limit).
  - 128 KB per file (seedling + sapling supported; tree files skipped).
  - Read-only — modifications by ProDOS are rejected with a write-protect
    error. Edit files on the host, then reclick the entry to refresh.

File type guessing:
  .bas → BAS ($FC)   .bin → BIN ($06)   .sys → SYS ($FF)
  .txt → TXT ($04)   .int → INT ($FA)   default → BIN
