// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Durable atomic file replace — pins src/AtomicFileReplace.h.
//
// Every write-back path in POM2 (DiskImage, Disk35Image, Block512Backing,
// ProDOSVolume, Settings, PrinterHistory, CassetteDevice, ImageWriter)
// commits through `replaceFileAtomic`, which since 2026-08-14 also flushes
// the data to the medium before publishing the rename — a power cut used to
// be able to leave a 0-byte file where the user's only copy of a disk image
// was.
//
// What this pins is the part a regression would actually break: the flush is
// on the SUCCESS path of every save, so a platform where it reports failure
// (or refuses to open the file) would turn every write-back into an error
// and lose the user's data far more reliably than the crash it guards
// against. Hence: the replace still succeeds, still swaps the bytes exactly,
// and a filesystem that cannot honour the flush is not treated as an I/O
// failure.

#include "AtomicFileReplace.h"

#include <cassert>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f && "open for write");
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    f.close();
    assert(f && "close after write");
}

std::vector<uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    assert(f && "open for read");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

std::vector<uint8_t> pattern(std::size_t n, uint8_t seed)
{
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>(seed + (i * 7));
    return v;
}

}  // namespace

int main()
{
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "pom2_atomic_replace";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    assert(!ec && "create scratch dir");

    // ── 1. Replace over an existing file ────────────────────────────────
    // A payload big enough to span many pages, so the flush has real work
    // to do rather than riding along in one dirty block.
    const fs::path dest = dir / "media.img";
    const fs::path tmp  = dir / "media.img.pom2tmp";
    const auto oldBytes = pattern(64 * 1024, 0x11);
    const auto newBytes = pattern(512 * 1024, 0xA5);
    writeFile(dest, oldBytes);
    writeFile(tmp, newBytes);

    ec.clear();
    assert(pom2::replaceFileAtomic(tmp, dest, ec) && "replace must succeed");
    assert(!ec && "no error code on success");
    assert(!fs::exists(tmp) && "temp file consumed by the rename");
    assert(readFile(dest) == newBytes && "destination holds the new bytes");

    // ── 2. Replace onto a path that does not exist yet ──────────────────
    const fs::path fresh    = dir / "new.img";
    const fs::path freshTmp = dir / "new.img.pom2tmp";
    const auto freshBytes   = pattern(4096, 0x5A);
    writeFile(freshTmp, freshBytes);
    ec.clear();
    assert(pom2::replaceFileAtomic(freshTmp, fresh, ec) && "create-by-rename");
    assert(!ec);
    assert(readFile(fresh) == freshBytes);

    // ── 3. An empty payload is still a legal save ───────────────────────
    // (ProDOSVolume exports 0-byte guest files; the flush must not choke.)
    const fs::path zero    = dir / "zero.bin";
    const fs::path zeroTmp = dir / "zero.bin.pom2tmp";
    writeFile(zeroTmp, {});
    ec.clear();
    assert(pom2::replaceFileAtomic(zeroTmp, zero, ec) && "empty file replace");
    assert(!ec);
    assert(fs::file_size(zero) == 0);

    // ── 4. The flush reports success on a plain regular file ────────────
    ec.clear();
    assert(pom2::syncFileContents(dest, ec) && "fsync a regular file");
    assert(!ec);

    // ── 5. ...and a genuinely missing file is an error, not a silent ok ─
    // The callers hand this their own just-written temp file, so "cannot
    // open it" means something is wrong with the save, not with fsync.
    ec.clear();
    assert(!pom2::syncFileContents(dir / "does-not-exist", ec));
    assert(ec && "error code set for the missing file");

    // ── 6. A failed replace leaves the destination untouched ────────────
    // (The source does not exist, so the rename cannot proceed.)
    const auto before = readFile(dest);
    ec.clear();
    assert(!pom2::replaceFileAtomic(dir / "missing.tmp", dest, ec));
    assert(ec && "error code set");
    assert(readFile(dest) == before && "destination survives a failed commit");

    // ── 7. Best-effort directory flush never throws or aborts ───────────
    pom2::syncParentDirectory(dest);
    pom2::syncParentDirectory(fs::path("relative-name-with-no-parent"));

    // ── 8. A symlink planted at the TEMP path must not be written through ─
    //
    // Callers vet the TARGET — the AI control server refuses a path outside
    // the working directory, refuses a symlink and demands a .pom2snap
    // extension — but `<target>.tmp` is derived afterwards and inherits none
    // of it, while ofstream(trunc) follows symlinks like any other open. A
    // file planted there redirected the write onto whatever it pointed at,
    // and the rename then moved the symlink away, leaving the destroyed
    // victim behind with nothing to show what had happened.
    {
        const fs::path victim  = dir / "precious.txt";
        const fs::path tmpLink = dir / "target.bin.tmp";
        {
            std::ofstream v(victim, std::ios::binary | std::ios::trunc);
            v << "do not overwrite me";
        }
        ec.clear();
        fs::remove(tmpLink, ec);
        ec.clear();               // a missing file is not a failure here
        fs::create_symlink(victim, tmpLink, ec);
        if (ec) {
            std::printf("  (skipped: this filesystem has no symlinks)\n");
        } else {
            std::error_code guard;
            const bool ok = pom2::prepareTempPath(tmpLink, guard);
            assert(ok && "a symlink at the temp path must be cleared, not honoured");
            assert(!fs::is_symlink(fs::symlink_status(tmpLink)));

            // And the victim is untouched — that is the whole point.
            std::ifstream in(victim, std::ios::binary);
            std::string kept((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            assert(kept == "do not overwrite me");
        }

        // A DIRECTORY there is refused, not deleted: it redirects nothing, and
        // removing it would be a bigger act than this is entitled to. Callers
        // (PrinterHistory's encoder among them) rely on the open failing.
        const fs::path tmpDir = dir / "target2.bin.tmp";
        ec.clear();
        fs::remove_all(tmpDir, ec);
        ec.clear();
        assert(fs::create_directory(tmpDir, ec) && !ec);
        std::error_code guard2;
        assert(!pom2::prepareTempPath(tmpDir, guard2));
        assert(fs::is_directory(tmpDir));
    }

    fs::remove_all(dir, ec);
    std::printf("atomic_file_replace: all assertions passed\n");
    return 0;
}
