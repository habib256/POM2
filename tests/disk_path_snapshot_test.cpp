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

// Disk-path snapshot locking smoke test.
//
// Pins the 2026-08-02 data race in MainWindow's media snapshots
// (applyProfile step 2, restartEmulationFromSettings step 0,
// renderDiskLibraryWindow's CurrentlyMounted build): they held a
// `const std::string&` handed back by DiskIICard::getDiskPath() /
// ProDOSBlockCard::getImagePath() with NO stateMutex. `controller->stop()`
// parks only the CPU worker; the AI control server's HTTP thread keeps
// serving /disk insert + eject (AiControlServer.cpp insertDisk/ejectDisk),
// and those reassign the very std::string the reference points into —
// aiServer->detach() happens later, in the teardown step.
//
// MainWindow itself needs GLFW/GL and can't be instantiated headlessly, so
// this pins the two properties the fix rests on, against the real card:
//
//   1. getDiskPath() aliases MUTABLE card state. A reference captured from
//      it is not a snapshot: a later insert/eject rewrites it under the
//      reader's feet. Copying BY VALUE is therefore mandatory, not stylistic.
//   2. A value copy taken under the same mutex the mutator holds only ever
//      observes a legal, complete path — the shape the fixed snapshot
//      builders use.
//
// (2) is a stress loop, so it is a smoke test rather than a proof; it is at
// its sharpest under -DPOM2_SANITIZE=thread, which flags the pre-fix
// unlocked read directly.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path tmpDsk(const std::string& tag)
{
    fs::path p = fs::temp_directory_path() / ("pom2_disk_path_snapshot_" + tag);
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage, 0);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

}  // namespace

int main()
{
    const fs::path a = tmpDsk("a.dsk");
    const fs::path b = tmpDsk("b.dsk");

    // ── 1. getDiskPath() returns a view of live, mutable state ──────────
    {
        DiskIICard card(6);
        assert(card.insertDisk(0, a.string()));

        const std::string& ref  = card.getDiskPath(0);   // the old snapshot
        const std::string  copy = card.getDiskPath(0);   // the fixed snapshot
        assert(ref == a.string());
        assert(copy == a.string());

        // Exactly what the AI control server's /disk handler does mid-snapshot.
        assert(card.insertDisk(0, b.string()));
        assert(ref == b.string());       // the reference followed the mutation
        assert(copy == a.string());      // the value copy did not

        card.ejectDisk(0);
        assert(ref.empty());             // …and can be emptied under a reader
        assert(copy == a.string());
    }

    // ── 2. Value snapshots under the shared lock only see legal paths ───
    {
        DiskIICard card(6);
        std::mutex stateMutex;           // stands in for EmulationController's
        std::atomic<long> mutations{0};

        // The writer runs a FIXED count and the reader runs until it is done,
        // rather than the reader running a fixed count and the writer until
        // told to stop. std::mutex is not fair: a reader that re-locks
        // immediately can hold the mutex continuously and starve the writer
        // to zero mutations, which under `ctest -j` made this abort roughly
        // one run in five. Bounding the WRITER makes the interleaving the
        // thing under test instead of the scheduler's generosity.
        constexpr long kMutations = 200;

        // "AI control HTTP thread": insert / eject in a tight loop.
        std::thread writer([&] {
            const std::string paths[2] = { a.string(), b.string() };
            for (long i = 0; i < kMutations; ++i) {
                {
                    std::lock_guard<std::mutex> lk(stateMutex);
                    if (i % 3 == 2) card.ejectDisk(0);
                    else            card.insertDisk(0, paths[i & 1]);
                }
                mutations.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        // "UI thread": build the media snapshot the way applyProfile /
        // restartEmulationFromSettings / the Disk Library now do — under the
        // lock, by value.
        long snapshots = 0;
        while (mutations.load(std::memory_order_relaxed) < kMutations ||
               snapshots < 20000) {
            std::string snap;
            bool loaded = false;
            {
                std::lock_guard<std::mutex> lk(stateMutex);
                loaded = card.isDiskLoaded(0);
                snap   = loaded ? std::string(card.getDiskPath(0))
                                : std::string();
            }
            // Whatever the writer was doing, the copy is a complete value:
            // either empty (ejected) or one of the two images.
            assert(!loaded || snap == a.string() || snap == b.string());
            assert(loaded  || snap.empty());
            ++snapshots;
            // Let the writer through — see the fairness note above.
            if ((snapshots & 0x3F) == 0) std::this_thread::yield();
        }

        writer.join();
        assert(snapshots >= 20000);
        assert(mutations.load(std::memory_order_relaxed) == kMutations);
    }

    std::error_code ec;
    fs::remove(a, ec);
    fs::remove(b, ec);

    std::printf("disk_path_snapshot_test: OK\n");
    return 0;
}
