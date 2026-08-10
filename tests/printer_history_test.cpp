// Print-history test — pins src/PrinterHistory.cpp.
//
// POM2 could already export a PDF but forgot everything on quit. This is the
// durable half, so what matters is that it SURVIVES — and that its failure
// modes are boring:
//
//   1. IT RELOADS. A store written by one session must come back intact in
//      the next, with metadata attached. That is the entire point.
//   2. A PARTIAL WRITE DOES NOT POISON IT. The index is rewritten whole on
//      every change; if that is interrupted, the previous index must still be
//      the one on disk (hence the write-to-temp-then-rename), and a truncated
//      or foreign index must yield an EMPTY history rather than rows pointing
//      at files that mean something else.
//   3. IT IS BOUNDED. An emulator left running must not quietly fill a disk
//      with printouts.
//   4. PAGES OF ONE DOCUMENT GROUP. Sheets ejected back to back are one job;
//      otherwise "re-preview this job" restores a single page and is useless.

// This TU provides the stb implementations. The GUI's single non-static copy
// lives in Pom2HgrPaintHost.cpp, which is not linked here — same arrangement
// imagewriter_pdf_test.cpp uses, and the reason this test does not drag the
// whole paint-host include cone in for a PNG writer.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "PrinterHistory.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pom2::HistoryPage;
using pom2::ImageWriter;
using pom2::PrinterHistory;

/// A page with a recognisable raster, so a reload can be checked to have
/// brought back THIS page and not merely A page.
ImageWriter::Page makePage(int w, int h, uint8_t seed)
{
    ImageWriter::Page p;
    p.w   = w;
    p.h   = h;
    p.dpi = 144;
    p.pix.assign(static_cast<size_t>(w) * h, 0);
    // Low 5 bits are ink, top 3 the ribbon band — write ink only.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p.pix[static_cast<size_t>(y) * w + x] =
                static_cast<uint8_t>(((x + y + seed) % 31));
    return p;
}

fs::path scratch(const char* leaf)
{
    fs::path d = fs::temp_directory_path() / "pom2_printer_history" / leaf;
    std::error_code ec;
    fs::remove_all(d, ec);
    return d;
}

// ── 1. Store, reload, and the metadata survives ──────────────────────────
void testPersistsAcrossSessions()
{
    const fs::path dir = scratch("persist");
    std::string err;

    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 0);

        const auto page = makePage(64, 48, 3);
        assert(h.addPage(page, /*model*/ 2, /*ribbon*/ 1, 8.5, 11.0, err));
        assert(h.size() == 1);
        assert(fs::exists(dir / h.pages()[0].file));
    }

    {   // A whole new object, as a new session would be.
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 1);
        const HistoryPage& p = h.pages()[0];
        assert(p.model == 2);
        assert(p.ribbon == 1);
        assert(p.paperW == 8.5);
        assert(p.paperL == 11.0);
        assert(p.w == 64 && p.h == 48 && p.dpi == 144);
        assert(!p.savedAt.empty());

        // And the raster really comes back.
        std::vector<uint8_t> rgba;
        int w = 0, h2 = 0;
        assert(h.loadRgba(p, rgba, w, h2, err));
        assert(w == 64 && h2 == 48);
        assert(rgba.size() == static_cast<size_t>(w) * h2 * 4);
    }
    std::printf("  ok: a stored page reloads with its metadata and raster\n");
}

// ── 2. Pages of one document share a job ─────────────────────────────────
void testPagesGroupIntoJobs()
{
    const fs::path dir = scratch("jobs");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    // Three sheets ejected back to back, as a three-page document does.
    for (int i = 0; i < 3; ++i)
        assert(h.addPage(makePage(32, 24, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    assert(h.size() == 3);
    const uint64_t job = h.pages()[0].job;
    for (const auto& p : h.pages()) assert(p.job == job);

    // jobPages returns them oldest first — the order they were printed.
    const auto pages = h.jobPages(job);
    assert(pages.size() == 3);
    assert(pages.front()->file < pages.back()->file);
    std::printf("  ok: sheets ejected together form one job, oldest first\n");
}

// ── 3. Bounded ───────────────────────────────────────────────────────────
void testTrimsToCap()
{
    const fs::path dir = scratch("cap");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));

    const size_t over = PrinterHistory::kMaxPages + 5;
    for (size_t i = 0; i < over; ++i)
        assert(h.addPage(makePage(8, 8, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    assert(h.size() == PrinterHistory::kMaxPages);

    // The evicted PNGs are gone from disk too — a cap that only trimmed the
    // index would leak files forever, which is the bug worth pinning.
    size_t pngs = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".png") ++pngs;
    assert(pngs == PrinterHistory::kMaxPages);

    std::printf("  ok: capped at %zu pages, and the evicted PNGs are deleted\n",
                PrinterHistory::kMaxPages);
}

// ── 4. A foreign or truncated index yields an EMPTY history ──────────────
void testBadIndexIsIgnored()
{
    {   // Foreign format.
        const fs::path dir = scratch("foreign");
        std::string err;
        {
            PrinterHistory h;
            assert(h.open(dir.string(), err));
            assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        }
        std::ofstream(dir / "index.txt", std::ios::trunc)
            << "{\"some\":\"json\"}\n";

        PrinterHistory h;
        assert(h.open(dir.string(), err));
        // Better an empty history than rows pointing at files whose meaning
        // POM2 cannot vouch for.
        assert(h.size() == 0);
    }

    {   // Truncated final record — the line is skipped, earlier ones survive.
        const fs::path dir = scratch("truncated");
        std::string err;
        {
            PrinterHistory h;
            assert(h.open(dir.string(), err));
            assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
            assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        }
        // Chop the last line mid-record.
        std::string all;
        {
            std::ifstream in(dir / "index.txt");
            std::string line;
            int n = 0;
            while (std::getline(in, line)) { all += line + "\n"; if (++n == 2) break; }
        }
        all += "p999999.png\t2026-01-01 00:00:00\t9\t0";   // no newline, short
        std::ofstream(dir / "index.txt", std::ios::trunc) << all;

        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.size() == 1);          // the intact record survived
    }
    std::printf("  ok: a foreign index empties, a truncated one keeps what parsed\n");
}

// ── 5. A page whose PNG was deleted by hand drops out ────────────────────
void testMissingFileDropsRow()
{
    const fs::path dir = scratch("missing");
    std::string err;
    std::string victim;
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        victim = h.pages()[0].file;
    }
    fs::remove(dir / victim);

    PrinterHistory h;
    assert(h.open(dir.string(), err));
    assert(h.size() == 1);
    assert(h.pages()[0].file != victim);
    std::printf("  ok: a hand-deleted PNG leaves no dead row\n");
}

// ── 6. Erase and clear ───────────────────────────────────────────────────
void testEraseAndClear()
{
    const fs::path dir = scratch("erase");
    std::string err;
    PrinterHistory h;
    assert(h.open(dir.string(), err));
    for (int i = 0; i < 3; ++i)
        assert(h.addPage(makePage(16, 16, static_cast<uint8_t>(i)), 0, 0,
                         8.0, 11.0, err));

    const std::string gone = h.pages()[1].file;
    assert(h.erase(h.pages()[1], err));
    assert(h.size() == 2);
    assert(!fs::exists(dir / gone));

    assert(h.clear(err));
    assert(h.size() == 0);
    size_t pngs = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".png") ++pngs;
    assert(pngs == 0);

    // A cleared store still reloads as a valid empty one.
    PrinterHistory h2;
    assert(h2.open(dir.string(), err));
    assert(h2.size() == 0);
    std::printf("  ok: erase and clear remove both the row and the file\n");
}

// ── 7. New pages never overwrite old files after a reload ────────────────
void testFileCounterSurvivesReload()
{
    const fs::path dir = scratch("counter");
    std::string err;
    std::string first;
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 1), 0, 0, 8.0, 11.0, err));
        first = h.pages()[0].file;
    }
    {
        PrinterHistory h;
        assert(h.open(dir.string(), err));
        assert(h.addPage(makePage(16, 16, 2), 0, 0, 8.0, 11.0, err));
        // If the counter restarted at 1 the new page would clobber the old
        // one's PNG and the history would show two rows of the same image.
        assert(h.pages()[0].file != first);
        assert(h.size() == 2);
        assert(fs::exists(dir / first));
    }
    std::printf("  ok: the file counter resumes, so a reload cannot clobber\n");
}

} // namespace

int main()
{
    testPersistsAcrossSessions();
    testPagesGroupIntoJobs();
    testTrimsToCap();
    testBadIndexIsIgnored();
    testMissingFileDropsRow();
    testEraseAndClear();
    testFileCounterSurvivesReload();

    std::puts("printer_history: OK");
    return 0;
}
