#!/usr/bin/env python3
"""Find and remove content-identical disk images in POM2's media folders.

POM2's Disk Library is a flat scan of disks_5.4/, disks_3.5/ and hdv/, so an
image that exists twice on disk shows up twice in the browser. This finds
byte-identical duplicates and keeps exactly one of each.

Keep rule, in order:
  1. A basename starting with "Copy (" never wins (accidental file-manager
     copies), unless every candidate in the group is one.
  2. Shortest full path wins. That is what makes the short hand-written name
     beat the verbose archive dump ("Congo Bongo.woz" over
     "Congo Bongo (1983)(Sega)[48K].woz") AND makes a per-game subfolder beat
     the same file sitting flat next to it ("woz/Dark Lord/Dark Lord side A.woz"
     over "woz/Dark Lord (1987)(Datasoft)(II+)(Side A)[64K].woz") — the
     subfolder path is shorter in every real case here.
  3. Lexicographic order breaks remaining ties, so runs are reproducible.

Grouping is by size first and content hash only within same-size groups, so a
1000-file library costs a handful of full reads rather than a thousand.

Dry-run by default. Pass --apply to actually delete.
"""

import argparse
import collections
import hashlib
import os
import sys

EXTS = {".dsk", ".do", ".po", ".nib", ".woz", ".d13", ".2mg", ".hdv"}
ROOTS = ["disks_5.4", "disks_3.5", "hdv"]


def scan(roots):
    """Return {size: [paths]} for every candidate image."""
    by_size = collections.defaultdict(list)
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            for f in filenames:
                if f.startswith(".") or os.path.splitext(f)[1].lower() not in EXTS:
                    continue
                p = os.path.join(dirpath, f)
                try:
                    by_size[os.path.getsize(p)].append(p)
                except OSError:
                    pass
    return by_size


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def duplicate_groups(by_size):
    """Only same-size files can be identical, so hash within size buckets."""
    groups = []
    for size, paths in by_size.items():
        if len(paths) < 2:
            continue
        by_hash = collections.defaultdict(list)
        for p in paths:
            try:
                by_hash[digest(p)].append(p)
            except OSError:
                pass
        for _, same in by_hash.items():
            if len(same) > 1:
                groups.append((size, sorted(same)))
    groups.sort(key=lambda g: g[1][0])
    return groups


def pick_keeper(paths):
    def is_copy(p):
        return os.path.basename(p).startswith("Copy (")

    candidates = [p for p in paths if not is_copy(p)] or list(paths)
    return min(candidates, key=lambda p: (len(p), p))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true",
                    help="actually delete (default is a dry run)")
    ap.add_argument("--root", action="append", default=None,
                    help="override the scanned roots (repeatable)")
    args = ap.parse_args()

    roots = args.root or ROOTS
    if not any(os.path.isdir(r) for r in roots):
        print("None of the media roots exist here: " + ", ".join(roots),
              file=sys.stderr)
        print("Run this from the repository root.", file=sys.stderr)
        return 1

    groups = duplicate_groups(scan(roots))
    if not groups:
        print("No duplicate disk images found.")
        return 0

    reclaimed = 0
    doomed = []
    for n, (size, paths) in enumerate(groups, 1):
        keeper = pick_keeper(paths)
        print(f"[{n:2d}] {size // 1024} KB")
        print(f"     KEEP   {keeper}")
        for p in paths:
            if p == keeper:
                continue
            print(f"     DELETE {p}")
            doomed.append(p)
            reclaimed += size

    print()
    print(f"{len(groups)} duplicate groups, {len(doomed)} redundant files, "
          f"{reclaimed / 1024 / 1024:.1f} MB reclaimable")

    if not args.apply:
        print("\nDry run. Re-run with --apply to delete the DELETE lines above.")
        return 0

    failed = 0
    for p in doomed:
        try:
            os.remove(p)
        except OSError as e:
            print(f"  ! failed to remove {p}: {e}", file=sys.stderr)
            failed += 1
    print(f"\nDeleted {len(doomed) - failed} files"
          f"{f', {failed} failed' if failed else ''}.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
