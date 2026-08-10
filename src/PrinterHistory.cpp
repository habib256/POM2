// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterHistory implementation. See the header for the on-disk layout and
// why the index is a tab-separated text file rather than JSON.

#include "PrinterHistory.h"

#include "Logger.h"

// Declarations only — the single non-static stb implementations live in
// Pom2HgrPaintHost.cpp, the same arrangement ImageWriterPdf.cpp documents.
#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pom2 {

namespace {

namespace fs = std::filesystem;

constexpr const char* kIndexName = "index.txt";
/// Version tag on the index's first line. An index POM2 does not recognise is
/// ignored rather than half-parsed — a stale format must not produce garbage
/// entries pointing at files that mean something else.
constexpr const char* kIndexMagic = "pom2-printer-history\t1";

/// Sheets ejected within this many seconds of each other belong to the same
/// print job. A multi-page document ejects its sheets seconds apart; a new
/// document minutes later is a new job.
constexpr int64_t kJobGapSeconds = 90;

int64_t nowEpoch()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string nowStamp()
{
    const std::time_t t = static_cast<std::time_t>(nowEpoch());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/// Strip anything that would break a tab-separated record. Cheap insurance:
/// the only free-text field is a timestamp POM2 formats itself today, but a
/// label field is the obvious next addition and this is where it would bite.
std::string sanitise(std::string s)
{
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

} // namespace

// ── Open / index ─────────────────────────────────────────────────────────

bool PrinterHistory::open(const std::string& dir, std::string& err)
{
    // Retarget the store only once the writer is idle: it caches `dir_` per
    // job, and swapping the directory under a queued page would file it in
    // the wrong place. Also drains, so nothing pending is dropped.
    stopWriter();

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    dir_ = dir;
    pages_.clear();
    nextJob_ = nextFile_ = 1;
    lastPageEpoch_ = 0;
    readIndex();          // absent / unreadable = empty history, not an error
    return true;
}

bool PrinterHistory::readIndex()
{
    std::ifstream in(fs::path(dir_) / kIndexName);
    if (!in) return false;

    std::string line;
    if (!std::getline(in, line) || line.rfind(kIndexMagic, 0) != 0) {
        pom2::log().warn("PrinterHistory",
                         "unrecognised index format — starting a fresh history");
        return false;
    }

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        HistoryPage p;
        std::string job, model, ribbon, pw, pl, w, h, dpi;
        // A truncated final line (killed mid-write) simply fails here and is
        // skipped, which is why the index is append-friendly plain text.
        if (!std::getline(ls, p.file, '\t'))    continue;
        if (!std::getline(ls, p.savedAt, '\t')) continue;
        if (!std::getline(ls, job, '\t'))       continue;
        if (!std::getline(ls, model, '\t'))     continue;
        if (!std::getline(ls, ribbon, '\t'))    continue;
        if (!std::getline(ls, pw, '\t'))        continue;
        if (!std::getline(ls, pl, '\t'))        continue;
        if (!std::getline(ls, w, '\t'))         continue;
        if (!std::getline(ls, h, '\t'))         continue;
        if (!std::getline(ls, dpi))             continue;

        try {
            p.job    = std::stoull(job);
            p.model  = std::stoi(model);
            p.ribbon = std::stoi(ribbon);
            p.paperW = std::stod(pw);
            p.paperL = std::stod(pl);
            p.w      = std::stoi(w);
            p.h      = std::stoi(h);
            p.dpi    = std::stoi(dpi);
        } catch (...) {
            continue;                    // malformed record, skip it
        }

        // Drop entries whose PNG has gone — a user who deleted files by hand
        // should not get a history full of dead rows.
        std::error_code ec;
        if (!fs::exists(fs::path(dir_) / p.file, ec)) continue;

        nextJob_ = std::max(nextJob_, p.job + 1);
        // File names are pNNNNNN.png; recover the counter so a new page never
        // overwrites an old one.
        if (p.file.size() > 5) {
            try {
                nextFile_ = std::max<uint64_t>(
                    nextFile_, std::stoull(p.file.substr(1)) + 1);
            } catch (...) { /* non-standard name: leave the counter alone */ }
        }
        pages_.push_back(std::move(p));
    }

    // Stored oldest-first; presented newest-first.
    std::reverse(pages_.begin(), pages_.end());
    return true;
}

bool PrinterHistory::writeIndex(std::string& err) const
{
    const fs::path tmp   = fs::path(dir_) / (std::string(kIndexName) + ".tmp");
    const fs::path final = fs::path(dir_) / kIndexName;

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "cannot write " + tmp.string(); return false; }
        out << kIndexMagic << "\n";
        // Oldest first on disk: appending is then the natural order, and a
        // reader that stops early still has the oldest records intact.
        for (auto it = pages_.rbegin(); it != pages_.rend(); ++it) {
            out << sanitise(it->file) << '\t' << sanitise(it->savedAt) << '\t'
                << it->job << '\t' << it->model << '\t' << it->ribbon << '\t'
                << it->paperW << '\t' << it->paperL << '\t'
                << it->w << '\t' << it->h << '\t' << it->dpi << '\n';
        }
        if (!out) { err = "write failed: " + tmp.string(); return false; }
    }

    // Rename over the old index: a crash mid-write then leaves the previous
    // index intact rather than a half-truncated one.
    std::error_code ec;
    fs::rename(tmp, final, ec);
    if (ec) {
        err = "cannot replace the index: " + ec.message();
        return false;
    }
    return true;
}

// ── Background writer ────────────────────────────────────────────────────

PrinterHistory::~PrinterHistory() { stopWriter(); }

void PrinterHistory::startWriter()
{
    if (writer_.joinable()) return;
    writer_ = std::thread(&PrinterHistory::writerLoop, this);
}

void PrinterHistory::stopWriter()
{
    if (!writer_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        writerQuit_ = true;
    }
    // The loop only returns once the queue is EMPTY, so this drains rather
    // than discards: a sheet ejected a frame before the user quit still lands
    // on disk.
    qCv_.notify_all();
    writer_.join();
    std::lock_guard<std::mutex> lk(qMtx_);
    writerQuit_ = false;
}

void PrinterHistory::writerLoop()
{
    for (;;) {
        PendingWrite job;
        std::string  dir;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait(lk, [this] { return writerQuit_ || !queue_.empty(); });
            if (queue_.empty()) return;         // asked to quit, nothing left
            // COPIED, not moved: the entry has to stay visible at the front
            // until its file exists, so loadRgba() can serve a sheet the user
            // clicked on before the encode finished.
            job = queue_.front();
            dir = dir_;
        }

        std::vector<uint8_t> rgba;
        ImageWriter::pageToRgba(job.page, rgba);
        const fs::path out = fs::path(dir) / job.file;
        if (rgba.size() < static_cast<size_t>(job.page.w) * job.page.h * 4 ||
            !stbi_write_png(out.string().c_str(), job.page.w, job.page.h, 4,
                            rgba.data(), job.page.w * 4)) {
            pom2::log().warn("PrinterHistory",
                             "cannot write " + out.string());
        }

        {
            std::lock_guard<std::mutex> lk(qMtx_);
            if (!queue_.empty()) queue_.pop_front();
        }
        qDoneCv_.notify_all();
        qCv_.notify_all();                      // a producer may want the room
    }
}

void PrinterHistory::flushPending() const
{
    std::unique_lock<std::mutex> lk(qMtx_);
    qDoneCv_.wait(lk, [this] { return queue_.empty(); });
}

size_t PrinterHistory::pendingWrites() const
{
    std::lock_guard<std::mutex> lk(qMtx_);
    return queue_.size();
}

// ── Storing ──────────────────────────────────────────────────────────────

bool PrinterHistory::addPage(const ImageWriter::Page& page, int model,
                             int ribbon, double paperW, double paperL,
                             std::string& err)
{
    if (dir_.empty()) { err = "history is not open"; return false; }
    if (page.w <= 0 || page.h <= 0 || page.pix.empty()) {
        err = "empty page";
        return false;
    }

    char name[32];
    std::snprintf(name, sizeof(name), "p%06llu.png",
                  static_cast<unsigned long long>(nextFile_));

    // Hand the sheet to the writer thread rather than encoding it here: this
    // runs on the ImGui render thread, once per ejected sheet, and a Letter
    // page at 144 dpi costs ~100-140 ms to convert and deflate — six to eight
    // dropped frames, four of them back to back on a form-feed catch-up.
    startWriter();
    {
        std::unique_lock<std::mutex> lk(qMtx_);
        // Back-pressure instead of unbounded RAM. Only ever waits when the
        // disk is genuinely slower than the printer, which is strictly better
        // than paying the encode on every single sheet.
        qCv_.wait(lk, [this] { return queue_.size() < kMaxPending; });
        queue_.push_back(PendingWrite{name, page});
    }
    qCv_.notify_all();

    const int64_t now = nowEpoch();
    HistoryPage p;
    p.file    = name;
    p.savedAt = nowStamp();
    // Sheets ejected close together are one document; a sheet much later is
    // a new one. Without this every page of a report is its own "job" and
    // re-preview becomes useless.
    p.job = (lastPageEpoch_ != 0 && now - lastPageEpoch_ <= kJobGapSeconds)
                ? nextJob_ - 1
                : nextJob_++;
    p.model  = model;
    p.ribbon = ribbon;
    p.paperW = paperW;
    p.paperL = paperL;
    p.w      = page.w;
    p.h      = page.h;
    p.dpi    = page.dpi;

    lastPageEpoch_ = now;
    ++nextFile_;
    pages_.insert(pages_.begin(), std::move(p));   // newest first

    trim(err);
    return writeIndex(err);
}

void PrinterHistory::trim(std::string& err)
{
    while (pages_.size() > kMaxPages) {
        const HistoryPage& oldest = pages_.back();
        std::error_code ec;
        fs::remove(fs::path(dir_) / oldest.file, ec);
        if (ec)
            pom2::log().warn("PrinterHistory",
                             "could not delete " + oldest.file + ": " + ec.message());
        pages_.pop_back();
    }
    (void)err;
}

// ── Reading back ─────────────────────────────────────────────────────────

bool PrinterHistory::loadRgba(const HistoryPage& p, std::vector<uint8_t>& rgba,
                              int& w, int& h, std::string& err) const
{
    // A sheet the writer has not reached yet is served straight from the
    // queue. The panel puts a freshly archived page on screen within a frame,
    // which is well inside the ~100 ms its encode takes.
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        for (const auto& q : queue_) {
            if (q.file != p.file) continue;
            ImageWriter::pageToRgba(q.page, rgba);
            w = q.page.w;
            h = q.page.h;
            return true;
        }
    }

    const fs::path full = fs::path(dir_) / p.file;
    int comp = 0;
    unsigned char* data = stbi_load(full.string().c_str(), &w, &h, &comp, 4);
    if (!data) {
        err = "cannot read " + full.string();
        return false;
    }
    rgba.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);
    return true;
}

std::vector<const HistoryPage*> PrinterHistory::jobPages(uint64_t job) const
{
    std::vector<const HistoryPage*> out;
    for (const auto& p : pages_)
        if (p.job == job) out.push_back(&p);
    // `pages_` is newest first; a job reads oldest first.
    std::reverse(out.begin(), out.end());
    return out;
}

// ── Deleting ─────────────────────────────────────────────────────────────

bool PrinterHistory::erase(const HistoryPage& p, std::string& err)
{
    // Never delete a file the writer is still about to create — it would be
    // resurrected moments later as an orphan the index does not mention.
    // A user-paced action can afford the wait; a per-sheet encode could not.
    flushPending();
    const std::string file = p.file;         // `p` may point into pages_
    std::error_code ec;
    fs::remove(fs::path(dir_) / file, ec);
    pages_.erase(std::remove_if(pages_.begin(), pages_.end(),
                                [&](const HistoryPage& q) { return q.file == file; }),
                 pages_.end());
    return writeIndex(err);
}

bool PrinterHistory::clear(std::string& err)
{
    flushPending();                          // see erase()
    for (const auto& p : pages_) {
        std::error_code ec;
        fs::remove(fs::path(dir_) / p.file, ec);
    }
    pages_.clear();
    lastPageEpoch_ = 0;
    return writeIndex(err);
}

} // namespace pom2
