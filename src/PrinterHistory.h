// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterHistory — completed printouts, kept across sessions.
//
// POM2 could already export a PDF, but it forgot everything the moment you
// quit: the sheet stack lives in `ImageWriter` and is capped at 32 pages. This
// is the durable half — every ejected sheet is written to disk as a PNG with
// the metadata needed to make sense of it later (which printer, which ribbon,
// what paper), and the panel can bring a past job back onto the platen.
//
// ── Layout on disk ────────────────────────────────────────────────────────
//
//   printouts/history/index.txt        one line per page, tab-separated
//   printouts/history/p000123.png      the page raster, 8-bit greyscale or
//                                      RGB when the ribbon was colour
//
// ── Why the index is not JSON ─────────────────────────────────────────────
//
// The plan said JSON. POM2 has no JSON *parser* — the AI control server only
// ever writes it — and pulling one in to read an index of a few dozen lines
// would be the tail wagging the dog. A tab-separated line format is trivial to
// write, trivial to parse, survives a truncated final line, and a user can
// read it in a terminal when something looks wrong. The one real cost is that
// a field containing a tab or a newline would corrupt a record, so the only
// free-text field (the label) is sanitised on the way in.
//
// ── What this deliberately does not do ────────────────────────────────────
//
// No thumbnails, no search, no compression beyond PNG's. A printout history is
// a few dozen pages; anything cleverer would be machinery in search of a
// problem.

#ifndef POM2_PRINTER_HISTORY_H
#define POM2_PRINTER_HISTORY_H

#include "ImageWriter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// One stored page. `file` is a bare filename inside the history directory,
/// never a path, so the store stays relocatable.
struct HistoryPage {
    std::string file;
    std::string savedAt;        ///< "YYYY-MM-DD HH:MM:SS", local time
    uint64_t    job     = 0;    ///< pages ejected without a gap share a job
    int         model   = 0;    ///< IwModel at the time
    int         ribbon  = 0;    ///< ImageWriter::Ribbon at the time
    double      paperW  = 0.0;  ///< inches
    double      paperL  = 0.0;
    int         w       = 0;    ///< raster size
    int         h       = 0;
    int         dpi     = 0;
};

class PrinterHistory
{
public:
    /// Largest number of pages kept. Older ones are deleted as new arrive —
    /// a printout is a few hundred KB, and an unbounded store on a machine
    /// left running is how an emulator ends up owning someone's disk.
    static constexpr size_t kMaxPages = 200;

    /// Point the store at `dir`, creating it, and read whatever index is
    /// there. A missing or unreadable index is not an error — it means an
    /// empty history. Returns false only when the directory cannot be made.
    bool open(const std::string& dir, std::string& err);
    bool isOpen() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    /// Store one ejected sheet. `jobGapSeconds` decides whether this page
    /// continues the previous job or starts a new one — sheets ejected back
    /// to back are one print job, a sheet an hour later is not.
    bool addPage(const ImageWriter::Page& page, int model, int ribbon,
                 double paperW, double paperL, std::string& err);

    /// Newest first — which is the order a history is read in.
    const std::vector<HistoryPage>& pages() const { return pages_; }
    size_t size() const { return pages_.size(); }

    /// Decode a stored page back to RGBA for display or re-export.
    bool loadRgba(const HistoryPage& p, std::vector<uint8_t>& rgba,
                  int& w, int& h, std::string& err) const;

    /// Every page of `job`, oldest first — what "re-preview this job" needs.
    std::vector<const HistoryPage*> jobPages(uint64_t job) const;

    /// Forget one page (and delete its PNG), or everything.
    bool erase(const HistoryPage& p, std::string& err);
    bool clear(std::string& err);

private:
    bool writeIndex(std::string& err) const;
    bool readIndex();
    void trim(std::string& err);

    std::string              dir_;
    std::vector<HistoryPage> pages_;      ///< newest first
    uint64_t                 nextJob_  = 1;
    uint64_t                 nextFile_ = 1;
    /// Wall-clock seconds since the last stored page, used to decide whether
    /// the next one continues the same job.
    int64_t                  lastPageEpoch_ = 0;
};

} // namespace pom2

#endif // POM2_PRINTER_HISTORY_H
