// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriter — see ImageWriter.h for the port provenance, the deliberate
// deviations from greg-kennedy/ImageWriter, and the page encoding.

#include "ImageWriter.h"

#include "hgrpaint/HgrFont.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace pom2 {

namespace {

// Paper sizes in PostScript points (iw_charmaps.h:62-77).
struct PaperDef { const char* name; int wPt; int hPt; };
constexpr PaperDef kPapers[] = {
    { "US Letter (8.5 x 11 in)",     612,  792 },
    { "US Legal (8.5 x 14 in)",      612, 1008 },
    { "ISO A4 (210 x 297 mm)",       595,  842 },
    { "ISO B5 (176 x 250 mm)",       499,  709 },
    { "Wide fanfold (14 x 11 in)",  1071,  792 },
    { "Ledger (11 x 17 in)",         792, 1224 },
    { "ISO A3 (297 x 420 mm)",       842, 1191 },
};
static_assert(sizeof(kPapers) / sizeof(kPapers[0]) ==
                  static_cast<size_t>(ImageWriter::PaperSize::Count),
              "kPapers must cover every PaperSize");

// Soft switch A (imagewriter.cpp:87-98).
constexpr uint8_t kSwitchACharsetMask    = 0x07;
constexpr uint8_t kSwitchAPerforationSkip= 0x10;
constexpr uint8_t kSwitchALfAfterCr      = 0x80;

// Ten ASCII positions the international charset switches (A-1..A-3) remap
// (imagewriter.cpp:450-460): # @ [ \ ] ` { | } ~.
constexpr uint8_t kIntlSlots[10] = {
    0x23, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e
};

// iw_charmaps.h:50-60, transcribed from Unicode to the CP437 code points
// the bundled 8x8 font is indexed by. Where CP437 has no glyph (O-slash,
// diaeresis) the closest ASCII stand-in is used — the alternative would be
// a blank cell.
constexpr uint8_t kIntlCharSets[8][10] = {
    { 0x23, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e }, // USA
    { 0x9c, 0x15, 0xf8, 0x87, 0x82, 0x97, 0x85, 0x95, 0x8a, 0x8d }, // Italian
    { 0x23, 0x40, 0x92, 0x4f, 0x8f, 0x60, 0x91, 0x6f, 0x86, 0x7e }, // Danish
    { 0x9c, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e }, // UK
    { 0x23, 0x15, 0x8e, 0x99, 0x9a, 0x60, 0x84, 0x94, 0x81, 0xe1 }, // German
    { 0x23, 0x40, 0x8e, 0x99, 0x8f, 0x60, 0x84, 0x94, 0x86, 0x7e }, // Swedish
    { 0x9c, 0x85, 0xf8, 0x87, 0x15, 0x60, 0x82, 0x97, 0x8a, 0x22 }, // French
    { 0x9c, 0x15, 0xad, 0xa5, 0xa8, 0x60, 0xf8, 0xa4, 0x87, 0x7e }, // Spanish
};

// ESC K n → ribbon band (imagewriter.cpp:935-949). n is an ASCII digit:
// 0 black, 1 yellow, 2 magenta, 3 cyan, 4 orange(red), 5 green, 6 purple.
constexpr uint8_t kRibbonBand[7] = { 7, 4, 1, 2, 5, 6, 3 };

const char* bandName(uint8_t band)
{
    switch (band & 7) {
        case 0: return "none";
        case 1: return "magenta";
        case 2: return "cyan";
        case 3: return "purple";
        case 4: return "yellow";
        case 5: return "orange";
        case 6: return "green";
        default: return "black";
    }
}

// Multi-byte ImageWriter parameters arrive as ASCII digit strings.
inline int paramDigit(uint8_t p)
{
    // Anything that is not an ASCII digit reads as 0: one corrupted byte
    // inside an ESC G/S/C count used to go negative, and the uint32_t cast
    // in setupBitImage turned that into ~4 G bytes of "graphics data" that
    // wedged the parser for the rest of the session.
    return (p >= '0' && p <= '9') ? static_cast<int>(p) - '0' : 0;
}

// ─── Mechanism speed (ImageWriter II Owner's Manual, "Specifications") ──
// 250 cps draft / 45 cps NLQ, both quoted at the 12 cpi default pitch —
// so the carriage crosses cps/cpi inches per second. Graphics passes are
// unidirectional (the head only prints left-to-right so the dot columns
// stay in register), which halves the effective rate. Paper transport is
// quoted as a 5 in/s slew.
constexpr double kDraftCps = 250.0;
constexpr double kNlqCps   = 45.0;
constexpr double kQuotedCpi = 12.0;
constexpr double kFeedIps  = 5.0;
/// Never bank more than this much mechanism time: a hidden window or a
/// long host stall must not dump half a page in one frame.
constexpr double kMaxCredit = 1.0;

} // namespace

const char* ImageWriter::paperSizeName(PaperSize s)
{
    const auto i = static_cast<size_t>(s);
    return i < static_cast<size_t>(PaperSize::Count) ? kPapers[i].name
                                                     : kPapers[0].name;
}

ImageWriter::ImageWriter(int dpi, PaperSize paper)
    : dpi_(std::clamp(dpi, kMinDpi, kMaxDpi)), paper_(paper)
{
    const auto i = static_cast<size_t>(paper_) <
                       static_cast<size_t>(PaperSize::Count)
                   ? static_cast<size_t>(paper_) : 0u;
    defaultPageWidth_  = kPapers[i].wPt / 72.0;
    defaultPageHeight_ = kPapers[i].hPt / 72.0;

    rebuildPage();
    resetPrinter();
}

ImageWriter::~ImageWriter()
{
    stopTrace();        // flushes the partial hex row + writes the footer
}

// ─────────────────────────────────────────────────────────────────────────
// Host-side configuration
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::setDpi(int dpi)
{
    const int d = std::clamp(dpi, kMinDpi, kMaxDpi);
    if (d == dpi_) return;
    dpi_ = d;
    rebuildPage();
    resetPrinter();
}

void ImageWriter::setPaperSize(PaperSize s)
{
    if (s == paper_ || static_cast<size_t>(s) >=
                           static_cast<size_t>(PaperSize::Count)) return;
    paper_ = s;
    const auto i = static_cast<size_t>(paper_);
    defaultPageWidth_  = kPapers[i].wPt / 72.0;
    defaultPageHeight_ = kPapers[i].hPt / 72.0;
    rebuildPage();
    resetPrinter();
}

void ImageWriter::rebuildPage()
{
    current_.w = std::max(1, static_cast<int>(defaultPageWidth_  * dpi_));
    current_.h = std::max(1, static_cast<int>(defaultPageHeight_ * dpi_));
    current_.pix.assign(static_cast<size_t>(current_.w) * current_.h, 0);
    current_.dpi = dpi_;
    ++revision_;
}

// ─────────────────────────────────────────────────────────────────────────
// Reset / paper handling (imagewriter.cpp:262-325, 1222-1252)
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::resetPrinter()
{
    color_        = 7 << 5;             // COLOR_BLACK
    curX_ = curY_ = 0.0;
    escSeen_ = fsSeen_ = false;
    escCmd_  = 0;
    numParam_ = neededParam_ = 0;
    topMargin_    = 0.0;
    // Practically every Apple II printer driver (including GS/OS) assumes
    // the ImageWriter's 1/4 inch left margin (imagewriter.cpp:281).
    leftMargin_   = 0.25;
    rightMargin_  = pageWidthIn_  = defaultPageWidth_;
    bottomMargin_ = pageHeightIn_ = defaultPageHeight_;
    lineSpacing_  = 1.0 / 6.0;
    cpi_          = 12.0;
    printRes_     = 2;                  // 12 cpi / 96 dpi graphics
    definedUnit_  = 96.0;
    // Reference sets STYLE_BOLD here to fatten a thin TrueType face; on a
    // dot-matrix cell that smears every glyph — see ImageWriter.h.
    style_        = 0;
    extraIntraSpace_ = 0.0;
    bitGraph_.remBytes = 0;
    bitGraph_.readBytesColumn = 0;
    hmi_          = -1.0;
    switcha_      = 0;                  // SWITCHA_CHARSET_US
    switchb_      = ' ';
    verticalDot_  = 0;
    numHorizTabs_ = 0;
    numVertTabs_  = 0;
    // Re-arm the CR/LF detector. `ESC c` is "initialize printer" — a new
    // job announcing itself — and it is the ONLY thing a guest can send
    // that re-arms this. Leaving the latch here scoped it to the host
    // session instead of the job: once one CR+LF driver had latched CR
    // "don't feed", every later `PR#n : LIST` in the same session printed
    // its whole listing overprinted onto a single black line, and nothing
    // in the guest could clear it — only the panel's power button.
    //
    // Print Shop's colour passes are safe: it separates them with a BARE
    // CR, never `ESC c`, so the latch it relies on survives its own job.
    // Both cases are pinned in testAutoLineFeedDetection.
    feedLatchedOff_ = false;
    crJustFed_      = false;

    selectDefaultMap();
    updateMetrics();
    updateSwitch();

    // Keep whatever is already on the platen. The reference discards it
    // (imagewriter.cpp:315) — it could afford to, because it wrote each
    // page out to disk as it went; here the sheet exists nowhere else, so
    // discarding is silent data loss. A short report with no trailing form
    // feed vanished the moment the next program sent its `ESC c` init.
    // Eject it instead: that is also what the paper does on a real desk —
    // you do not get the sheet back by pressing reset.
    newPage(!currentPageBlank(), true);
}

void ImageWriter::resetPrinterHard()
{
    // Power cycle — whatever was still in the input buffer is gone with it.
    pending_.clear();
    pendingHead_ = 0;
    credit_      = 0.0;
    stalledFor_  = 0.0;           // and so is the stall watchdog
    catchUp_     = false;         // and the backlog it was chasing
    resetPrinter();               // re-arms the CR/LF detector
}

void ImageWriter::clearAll()
{
    pages_.clear();
    droppedPages_ = 0;
    bytesIn_      = 0;
    resetPrinterHard();
}

void ImageWriter::selectDefaultMap()
{
    // The bundled font is already CP437-indexed, so the base map is the
    // identity (the reference walks a CP437→Unicode table for FreeType,
    // imagewriter.cpp:345-362).
    for (int i = 0; i < 256; ++i) curMap_[i] = static_cast<uint8_t>(i);
}

void ImageWriter::updateSwitch()
{
    // imagewriter.cpp:447-479.
    const int charmap = switcha_ & kSwitchACharsetMask;
    for (int i = 0; i < 10; ++i)
        curMap_[kIntlSlots[i]] = kIntlCharSets[charmap][i];

    if (switcha_ & kSwitchAPerforationSkip) {
        topMargin_    = 0.25;
        bottomMargin_ = pageHeightIn_ - 0.25;
    } else {
        topMargin_    = 0.0;
        bottomMargin_ = pageHeightIn_;
    }

    // Switch B-6 selects whether bit 7 reaches the character generator.
    msb_ = (switchb_ & 32) ? 0 : 255;
}

void ImageWriter::updateMetrics()
{
    // Effective pitch, distilled from the reference's font-sizing block
    // (imagewriter.cpp:394-425) with the FreeType point maths dropped —
    // only `actcpi` survives into a dot-matrix cell.
    actcpi_ = cpi_;
    if (!(style_ & kStyleProp)) {
        if (cpi_ == 10.0 && (style_ & kStyleCondensed)) actcpi_ = 17.14;
        if (cpi_ == 12.0 && (style_ & kStyleCondensed)) actcpi_ = 20.0;
    } else if (style_ & kStyleCondensed) {
        actcpi_ *= 2.0;
    }
    if (style_ & kStyleDoubleWidth) actcpi_ /= 2.0;
    if (actcpi_ <= 0.0) actcpi_ = 12.0;
}

void ImageWriter::newPage(bool save, bool resetx)
{
    if (trace_) {
        traceFlushRow();
        std::fprintf(trace_,
            "[%8.3f] PAGE %s (head was at %.2f\" x %.2f\", %zu on the stack)\n",
            traceClock_, save ? "sheet ejected" : "sheet restarted",
            curX_, curY_, pages_.size());
        std::fflush(trace_);
    }
    if (save) {
        if (pages_.size() >= kMaxPages) {
            pages_.erase(pages_.begin());
            ++droppedPages_;
        }
        pages_.push_back(current_);
        // Monotonic (unlike pages_.size(), which the cap holds at 32) so
        // the catch-up drain can budget ejects — each one copies a whole
        // page raster, so a form-feed storm is dear per byte.
        ++sheetsEjected_;
    }
    if (resetx) curX_ = leftMargin_;
    curY_ = topMargin_;
    std::fill(current_.pix.begin(), current_.pix.end(), uint8_t{0});
    ++revision_;
}

void ImageWriter::formFeed()
{
    // FORM FEED button — don't eject a sheet nothing was printed on
    // (imagewriter.cpp:1606-1613).
    newPage(!currentPageBlank(), true);
}

bool ImageWriter::currentPageBlank() const
{
    for (uint8_t v : current_.pix)
        if (v != 0) return false;
    return true;
}

void ImageWriter::lineFeed()
{
    curY_ += lineSpacing_;
    // Reverse feeds (ESC r) stop at the top edge of the sheet — the head
    // position must never walk off the raster into negative territory.
    if (curY_ < 0.0) curY_ = 0.0;
    if (curY_ > bottomMargin_ - lineSpacing_) newPage(true, false);
}

// ─────────────────────────────────────────────────────────────────────────
// Dot plotting
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::fillDots(double xInch, double yInch,
                           double wInch, double hInch)
{
    if (current_.pix.empty()) return;

    int x0 = static_cast<int>(std::floor(xInch * dpi_ + 0.5));
    int y0 = static_cast<int>(std::floor(yInch * dpi_ + 0.5));
    int x1 = static_cast<int>(std::floor((xInch + wInch) * dpi_ + 0.5));
    int y1 = static_cast<int>(std::floor((yInch + hInch) * dpi_ + 0.5));
    if (x1 <= x0) x1 = x0 + 1;          // a dot is never sub-pixel
    if (y1 <= y0) y1 = y0 + 1;

    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, current_.w); y1 = std::min(y1, current_.h);
    if (x0 >= x1 || y0 >= y1) return;

    const uint8_t ink = static_cast<uint8_t>(color_ | 0x1F);
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = current_.pix.data() + static_cast<size_t>(y) * current_.w;
        for (int x = x0; x < x1; ++x) row[x] |= ink;
    }
    ++revision_;
}

// ─────────────────────────────────────────────────────────────────────────
// Text
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::renderGlyph(uint8_t ch)
{
    // An ImageWriter draft cell is 8 dots wide at the pitch's density and
    // 8 pins tall at 1/72 in spacing; the bundled font is 7 px + a 1 px
    // inter-character gap, so it drops straight into that cell.
    const double cellW = 1.0 / actcpi_;
    const double dotW  = cellW / 8.0;
    double dotH = 1.0 / 72.0;
    double top  = curY_;

    if (style_ & (kStyleSuperscript | kStyleSubscript | kStyleHalfHeight)) {
        dotH *= 2.0 / 3.0;
        // Superscript hugs the ascender line; the other two sit on the
        // baseline of the full-height cell.
        if (!(style_ & kStyleSuperscript))
            top += (8.0 / 72.0) - 8.0 * dotH;
    }

    // Bold on a real head is a second pass shifted half a dot — here the
    // dot is simply 1.5x wide, which is what that pass leaves on paper.
    const double inkW = (style_ & kStyleBold) ? dotW * 1.5 : dotW;

    const uint8_t glyph = curMap_[ch];
    for (int gy = 0; gy < hgrpaint::kBBFontGlyphH; ++gy) {
        for (int gx = 0; gx < hgrpaint::kBBFontGlyphW; ++gx) {
            if (!hgrpaint::bbFontPixel(glyph, gx, gy)) continue;
            // Italics shear the cell by one dot over its height, matching
            // the reference's 0.20 FreeType x-shear (imagewriter.cpp:437-442).
            const double shear = (style_ & kStyleItalics)
                               ? dotW * (7 - gy) * (1.0 / 7.0) : 0.0;
            fillDots(curX_ + gx * dotW + shear, top + gy * dotH, inkW, dotH);
        }
    }
}

void ImageWriter::printChar(uint8_t ch)
{
    ++bytesIn_;
    // Rolling raw capture — drops the oldest half when full so a runaway
    // job can't grow it without bound but the recent stream survives.
    if (raw_.size() >= kRawCaptureBytes)
        raw_.erase(raw_.begin(),
                   raw_.begin() + static_cast<std::ptrdiff_t>(raw_.size() / 2));
    raw_.push_back(ch);
    if (trace_) traceByte(ch);
    printCharInternal(ch);
}

void ImageWriter::printCharInternal(uint8_t ch)
{
    // Bit 7 is masked for text but never for graphics data
    // (imagewriter.cpp:1260-1263).
    if (msb_ != 255 && bitGraph_.remBytes == 0) ch &= 0x7F;

    if (bitGraph_.remBytes > 0) { printBitGraph(ch); return; }
    if (processCommandChar(ch))  return;

    if (ch == 0x01) ch = 0x20;

    const double lineStart = curX_;
    renderGlyph(ch);

    // Slashed zero when soft switch B-1 is closed (imagewriter.cpp:1325).
    if ((switchb_ & 1) && ch == '0') {
        const uint8_t saved = curMap_['0'];
        curMap_['0'] = curMap_['/'];
        renderGlyph('0');
        curMap_['0'] = saved;
    }

    double advance = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;
    advance += extraIntraSpace_;
    curX_ += advance;

    if (style_ & kStyleUnderline) {
        // One pin below the cell, across the character just printed.
        fillDots(lineStart, curY_ + 8.0 / 72.0,
                 curX_ - lineStart, 1.0 / 72.0);
    }

    // Wrap when the next character would cross the right margin.
    if ((curX_ + advance) > rightMargin_) {
        curX_ = leftMargin_;
        lineFeed();
    }
}

void ImageWriter::printBytes(const uint8_t* data, size_t n)
{
    if (!data) return;
    for (size_t i = 0; i < n; ++i) printChar(data[i]);
}

// ─────────────────────────────────────────────────────────────────────────
// Trace log
// ─────────────────────────────────────────────────────────────────────────

bool ImageWriter::startTrace(const std::string& path, std::string& err)
{
    stopTrace();
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    trace_ = std::fopen(path.c_str(), "w");
    if (!trace_) {
        err = "cannot open " + path + " for writing";
        return false;
    }
    tracePath_   = path;
    traceOffset_ = 0;
    traceRowLen_ = 0;
    std::fprintf(trace_,
        "# POM2 ImageWriter II trace\n"
        "# Every byte the printer consumed, decoded. Columns:\n"
        "#   [t]      seconds of mechanism time since the trace opened\n"
        "#   RX       hex dump of the input stream (offset = byte index)\n"
        "#   CMD      a completed escape sequence, with its parameters\n"
        "#   GFX      bit-image setup (density / columns / bytes per column)\n"
        "#   PAGE     sheet ejected\n"
        "#   HOST     host-side event (queue depth, BUSY, stalls)\n"
        "#\n"
        "# If a printout is noise, look at CMD: a driver talking another\n"
        "# printer's dialect shows up as commands this printer never got\n"
        "# (or as RX bytes that should have been graphics data).\n\n");
    std::fflush(trace_);
    return true;
}

void ImageWriter::stopTrace()
{
    if (!trace_) return;
    traceFlushRow();
    std::fprintf(trace_, "# trace closed after %llu bytes\n",
                 static_cast<unsigned long long>(traceOffset_));
    std::fclose(trace_);
    trace_ = nullptr;
}

void ImageWriter::traceEvent(const char* fmt, ...)
{
    if (!trace_) return;
    traceFlushRow();
    std::fprintf(trace_, "[%8.3f] HOST ", traceClock_);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(trace_, fmt, ap);
    va_end(ap);
    std::fputc('\n', trace_);
    std::fflush(trace_);
}

void ImageWriter::traceFlushRow()
{
    if (!trace_ || traceRowLen_ == 0) return;
    std::fprintf(trace_, "[%8.3f] RX   %06llX  ", traceClock_,
                 static_cast<unsigned long long>(traceOffset_ - traceRowLen_));
    for (int i = 0; i < 16; ++i) {
        if (i < traceRowLen_) std::fprintf(trace_, "%02X ", traceRow_[i]);
        else                  std::fprintf(trace_, "   ");
        if (i == 7) std::fputc(' ', trace_);
    }
    std::fputc('|', trace_);
    for (int i = 0; i < traceRowLen_; ++i) {
        const uint8_t c = traceRow_[i] & 0x7F;
        std::fputc((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.', trace_);
    }
    std::fprintf(trace_, "|\n");
    traceRowLen_ = 0;
    std::fflush(trace_);
}

void ImageWriter::traceByte(uint8_t ch)
{
    if (!trace_) return;
    traceRow_[traceRowLen_++] = ch;
    ++traceOffset_;
    if (traceRowLen_ == 16) traceFlushRow();
}

void ImageWriter::traceCommand()
{
    if (!trace_) return;
    traceFlushRow();
    const bool isFs = (escCmd_ & 0x800) != 0;
    const uint8_t c = static_cast<uint8_t>(escCmd_ & 0xFF);
    std::string params;
    for (uint8_t i = 0; i < numParam_; ++i) {
        const uint8_t p = params_[i] & 0x7F;
        params += (p >= 0x20 && p < 0x7F) ? static_cast<char>(p) : '.';
    }
    std::fprintf(trace_, "[%8.3f] CMD  %s %c ($%02X)%s%s\n", traceClock_,
                 isFs ? "US " : "ESC",
                 (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?', c,
                 params.empty() ? "" : "  params=", params.c_str());
    std::fflush(trace_);
}

// ─────────────────────────────────────────────────────────────────────────
// Mechanism pacing — bytes arrive at bus speed, dots land at head speed
// ─────────────────────────────────────────────────────────────────────────

const char* ImageWriter::ribbonName(Ribbon r)
{
    return r == Ribbon::Black ? "Black (single band)"
                              : "Four-colour (yellow/magenta/cyan/black)";
}

const char* ImageWriter::autoFeedName(AutoFeed m)
{
    switch (m) {
        case AutoFeed::On:  return "Always (printer supplies the feed)";
        case AutoFeed::Off: return "Never (the guest supplies it)";
        default:            return "Auto (follow what the guest sends)";
    }
}

void ImageWriter::setAutoFeedMode(AutoFeed m)
{
    if (m >= AutoFeed::Count) m = AutoFeed::Auto;
    feedMode_       = m;
    feedLatchedOff_ = false;      // re-arm the detector on any change
    crJustFed_      = false;
}

const char* ImageWriter::speedName(Speed s)
{
    switch (s) {
        case Speed::Draft: return "Draft (250 cps)";
        case Speed::NLQ:   return "Near letter quality (45 cps)";
        default:           return "Instant (no mechanism delay)";
    }
}

void ImageWriter::setSpeed(Speed s)
{
    if (s >= Speed::Count) s = Speed::Draft;
    speed_ = s;
    // Switching to Instant must not strand a half-printed job.
    if (speed_ == Speed::Instant) flushPending();
}

void ImageWriter::queueBytes(const uint8_t* data, size_t n)
{
    if (!data || n == 0) return;
    pending_.insert(pending_.end(), data, data + n);

    // Past kMaxBacklog the mechanism gives up on Draft/NLQ pacing — but it
    // catches up in tick(), at a bounded rate. Printing it here would run
    // the whole backlog on the UI thread in one frame (see kMaxBacklog).
    if (pendingBytes() > kMaxBacklog) catchUp_ = true;

    // A guest that sustainably outruns even the catch-up rate — a form
    // feed loop, where every one-byte FF asks for a whole sheet — still
    // may not grow the heap without bound. Past the hard ceiling the
    // oldest input is dropped, which is the same rule the page stack
    // (kMaxPages / droppedPageCount) and the SSC tap spool already follow:
    // bound the memory, count what was lost, and say so in the trace.
    // Truncating a printout is bad; freezing the emulator is worse.
    if (pendingBytes() > kHardBacklog) {
        const size_t drop = pendingBytes() - kHardBacklog;
        pendingHead_  += drop;
        droppedInput_ += drop;
        traceEvent("HOST: backlog over %zu KiB — dropped %zu input byte%s "
                   "(%zu total)", kHardBacklog >> 10, drop,
                   drop == 1 ? "" : "s", droppedInput_);
        // Advancing the cursor only makes the bytes unreachable — the
        // vector still holds them. Compact, or the "hard ceiling" bounds
        // nothing at all between two ticks.
        compactPending();
    }
}

void ImageWriter::compactPending()
{
    if (pendingHead_ >= pending_.size()) {
        pending_.clear();
        pendingHead_ = 0;
    } else if (pendingHead_ >= 8192) {
        // Compact instead of growing without bound on a long job.
        pending_.erase(pending_.begin(),
                       pending_.begin() +
                           static_cast<std::ptrdiff_t>(pendingHead_));
        pendingHead_ = 0;
    }
}

void ImageWriter::flushPending()
{
    while (pendingHead_ < pending_.size()) printChar(pending_[pendingHead_++]);
    pending_.clear();
    pendingHead_ = 0;
    credit_      = 0.0;
    stalledFor_  = 0.0;   // the queue is gone; don't arm the next job's
                          // watchdog with time this one spent stalled
    catchUp_     = false; // nothing left to catch up on
}

double ImageWriter::byteCost(uint8_t ch) const
{
    if (speed_ == Speed::Instant) return 0.0;

    const double cps = (speed_ == Speed::NLQ) ? kNlqCps : kDraftCps;
    const double ips = cps / kQuotedCpi;      // carriage inches per second

    // Bit-image data: the byte is a dot column (or a third of one on the
    // LQ's 24-pin pitches), not a glyph. Same head, half the sweep rate.
    if (bitGraph_.remBytes > 0) {
        const double colsPerSec = (ips * 0.5) * bitGraph_.horizDens;
        const double perColumn  = (colsPerSec > 0.0) ? 1.0 / colsPerSec : 0.0;
        const uint8_t perCol    = bitGraph_.bytesColumn ? bitGraph_.bytesColumn : 1;
        return perColumn / perCol;
    }

    // Mid-escape-sequence bytes (the command letter and its ASCII digit
    // parameters) never move the mechanism — they land in a register.
    if (escSeen_ || fsSeen_ || numParam_ < neededParam_) return 0.0;

    switch (ch & 0x7F) {
        case 0x0D: {   // CR — carriage back to the left margin
            // Draft is bidirectional (the head prints on the return
            // sweep), so a CR costs only the direction change; NLQ is
            // unidirectional and pays the full slew back.
            double t = (speed_ == Speed::NLQ)
                     ? std::max(0.0, curX_ - leftMargin_) / ips
                     : 0.02;
            // Paper transport time is positive in both directions (ESC r
            // makes lineSpacing_ negative; a negative cost *credited* the
            // pacing budget and dumped the whole queue in one frame).
            if (autoFeedActive()) t += std::fabs(lineSpacing_) / kFeedIps;
            return t;
        }
        case 0x0A:     // LF — one line of paper transport
            return std::fabs(lineSpacing_) / kFeedIps;
        case 0x0C:     // FF — slew whatever is left of the sheet
            return std::max(0.5, bottomMargin_ - curY_) / kFeedIps;
        default:
            break;
    }
    if ((ch & 0x7F) < 0x20) return 0.0;        // other control codes
    return 1.0 / cps;                          // one printed character
}

void ImageWriter::tick(double dt)
{
    if (pendingHead_ >= pending_.size()) {
        pending_.clear();
        pendingHead_ = 0;
        credit_      = 0.0;
        stalledFor_  = 0.0;   // idle printer: the next job starts fresh
        catchUp_     = false;
        return;
    }
    if (speed_ == Speed::Instant) { flushPending(); return; }

    // Behind by more than kMaxBacklog: pacing is suspended and the head
    // runs flat out — but only for a bounded slice of this tick, so the
    // window keeps repainting while it catches up. It stays armed until
    // the queue is EMPTY, not merely back under the threshold: stopping
    // at the threshold would hand ~512 KiB back to the 250 cps model and
    // spend half an hour of wall clock printing it, which is neither what
    // the memory bound wanted nor what the user is waiting for.
    if (catchUp_) {
        const size_t byteBudget  = pendingHead_ + kCatchUpBytes;
        const size_t sheetBudget = sheetsEjected_ + kCatchUpSheets;
        while (pendingHead_ < pending_.size() &&
               pendingHead_ < byteBudget &&
               sheetsEjected_ < sheetBudget) {
            printChar(pending_[pendingHead_++]);
        }
        credit_     = 0.0;
        stalledFor_ = 0.0;   // nothing is unaffordable while un-paced
        if (pendingHead_ >= pending_.size()) catchUp_ = false;
        compactPending();
        return;
    }

    // Bank the elapsed time, capped so a hidden window or a long host
    // stall doesn't dump half a page in one frame. The cap has to leave
    // room for the byte at the head of the queue: a form feed near the top
    // of a Letter sheet costs 2.2 s of paper transport, and a flat 1 s cap
    // meant that byte could NEVER be afforded — the queue stalled forever,
    // BUSY stayed asserted, and the guest spun in its firmware ACK loop
    // (Print Shop froze on every page eject).
    if (dt > 0.0) {
        traceClock_ += dt;
        const double head = byteCost(pending_[pendingHead_]);
        credit_ = std::min(credit_ + dt, std::max(kMaxCredit, head));
    }

    const size_t before = pendingHead_;
    while (pendingHead_ < pending_.size()) {
        const uint8_t ch   = pending_[pendingHead_];
        const double  cost = byteCost(ch);     // state-dependent: read first
        if (cost > credit_) break;
        credit_ -= cost;
        ++pendingHead_;
        printChar(ch);
    }

    // Watchdog. Nothing should ever be unaffordable for this long — but a
    // wedged printer takes the guest down with it (it waits on ACK), so a
    // cost-model mistake must degrade to "printed late", never to a hang.
    if (pendingHead_ == before && dt > 0.0) {
        stalledFor_ += dt;
        // The threshold has to clear the dearest byte the cost model can
        // legitimately produce, or the watchdog fires on a byte that was
        // merely SLOW. `ESC H 9999` + FF is 69" of paper at 5 ips = 13.9 s
        // of honest mechanism time, and a flat 10 s cap cut it short and
        // logged a STALL for a printer that was working correctly. The
        // credit cap already grows to `head` (see above), so waiting that
        // long is the paced answer; the watchdog is only for a cost the
        // model can never afford at all.
        const double patience =
            std::max(kStallSeconds, byteCost(pending_[pendingHead_]) * 1.5);
        if (stalledFor_ >= patience) {
            const uint8_t ch = pending_[pendingHead_];
            traceEvent("STALL: byte $%02X unaffordable for %.1f s "
                       "(cost %.3f s, credit %.3f s) — forcing it through",
                       ch, stalledFor_, byteCost(ch), credit_);
            ++pendingHead_;
            printChar(ch);
            credit_     = 0.0;
            stalledFor_ = 0.0;
        }
    } else {
        stalledFor_ = 0.0;
    }

    compactPending();
}

// ─────────────────────────────────────────────────────────────────────────
// Bit-image graphics (imagewriter.cpp:1432-1603)
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::setupBitImage(uint8_t dens, uint32_t numCols)
{
    // 0-7 = ImageWriter II pitches (8 pins/column, 72 dpi vertical),
    // 8-15 = ImageWriter LQ (24 pins/column over three bytes, 216 dpi).
    static constexpr uint16_t kHoriz[16] = {
        72, 80, 96, 107, 120, 136, 144, 160,
        144, 160, 192, 216, 240, 272, 288, 320
    };
    if (dens > 15) return;              // reference logs and drops

    bitGraph_.horizDens   = kHoriz[dens];
    bitGraph_.vertDens    = (dens < 8) ? 72 : 216;
    bitGraph_.adjacent    = true;
    bitGraph_.bytesColumn = (dens < 8) ? 1 : 3;
    bitGraph_.remBytes    = numCols * bitGraph_.bytesColumn;
    bitGraph_.readBytesColumn = 0;

    if (trace_) {
        traceFlushRow();
        std::fprintf(trace_,
            "[%8.3f] GFX  %u dpi x %u dpi, %u columns, %u byte%s/column "
            "(%u data bytes follow) at %.2f\"\n",
            traceClock_, bitGraph_.horizDens, bitGraph_.vertDens, numCols,
            bitGraph_.bytesColumn, bitGraph_.bytesColumn == 1 ? "" : "s",
            bitGraph_.remBytes, curX_);
        std::fflush(trace_);
    }
}

void ImageWriter::printBitGraph(uint8_t ch)
{
    bitGraph_.column[bitGraph_.readBytesColumn++] = ch;
    --bitGraph_.remBytes;

    // Only paint once a whole column has arrived.
    if (bitGraph_.readBytesColumn < bitGraph_.bytesColumn) return;

    const double oldY  = curY_;
    const double dotH  = 1.0 / bitGraph_.vertDens;
    const double dotW  = 1.0 / bitGraph_.horizDens;

    // ESC t shifts the LQ column down by n/216 in (imagewriter.cpp:1574-1577).
    if (printRes_ > 7 && verticalDot_ != 0)
        curY_ += static_cast<double>(verticalDot_) / bitGraph_.vertDens;

    for (uint8_t i = 0; i < bitGraph_.bytesColumn; ++i) {
        for (unsigned j = 1; j < 256u; j <<= 1) {   // pin 1 = bit 0 = top
            if (bitGraph_.column[i] & j) fillDots(curX_, curY_, dotW, dotH);
            curY_ += dotH;
        }
    }

    curY_ = oldY;
    bitGraph_.readBytesColumn = 0;
    curX_ += dotW;
}

// ─────────────────────────────────────────────────────────────────────────
// Command interpreter (imagewriter.cpp:497-1217, ported verbatim)
// ─────────────────────────────────────────────────────────────────────────

bool ImageWriter::processCommandChar(uint8_t ch)
{
    // "The previous byte was a CR we line-fed for" — only an LF landing
    // immediately after one counts as the guest supplying its own feed.
    const bool wasCrFed = crJustFed_;
    crJustFed_ = false;

    // ── Phase 1: the byte right after ESC / US selects the command ──────
    if (escSeen_ || fsSeen_) {
        escCmd_ = ch;
        if (fsSeen_) escCmd_ |= 0x800;
        escSeen_ = fsSeen_ = false;
        numParam_ = 0;

        switch (escCmd_) {
        case 0x21: case 0x22: case 0x24: case 0x2b: case 0x2e:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
        case 0x35: case 0x36: case 0x3c: case 0x3e: case 0x3f:
        case 0x41: case 0x42: case 0x45: case 0x4d: case 0x4e:
        case 0x4f: case 0x50: case 0x51: case 0x57: case 0x58:
        case 0x59: case 0x63: case 0x65: case 0x66: case 0x6b:
        case 0x6d: case 0x6e: case 0x6f: case 0x70: case 0x71:
        case 0x72: case 0x77: case 0x78: case 0x79: case 0x7a:
            neededParam_ = 0;
            break;
        case 0x3d:  // ESC = n  internal font ID
        case 0x40:  // ESC @ n  select output bin
        case 0x4b:  // ESC K n  select printing colour
        case 0x61:  // ESC a n  select font
        case 0x6c:  // ESC l n  insert CR before LF/FF
        case 0x73:  // ESC s n  intercharacter space
        case 0x74:  // ESC t n  shift printing down n/216 in
        case 0x833: // US n     feed n blank lines
            neededParam_ = 1;
            break;
        case 0x44:  // ESC D nn  close (set) soft switches
        case 0x54:  // ESC T nn  line spacing nn/144 in
        case 0x5a:  // ESC Z nn  open (clear) soft switches
            neededParam_ = 2;
            break;
        case 0x4c:  // ESC L nnn  left margin at column nnn
        case 0x67:  // ESC g nnn  graphics, nnn*8 data bytes
        case 0x75:  // ESC u nnn  add one tab stop
            neededParam_ = 3;
            break;
        case 0x28:  // ESC ( nnn,  set horizontal tabs
            numHorizTabs_ = 0;
            [[fallthrough]];
        case 0x29:  // ESC ) nnn,  delete horizontal tabs
        case 0x43:  // ESC C nnnn  hi-res graphics
        case 0x47:  // ESC G nnnn  graphics
        case 0x46:  // ESC F nnnn  head nnnn dots from left margin
        case 0x48:  // ESC H nnnn  page length nnnn/144
        case 0x53:  // ESC S nnnn  graphics
        case 0x52:  // ESC R nnn c repeat character
        case 0x68:  // ESC h nnnn  head nnnn hi-res dots from left margin
            neededParam_ = 4;
            break;
        case 0x56:  // ESC V nnnn c   repeat dot column
            neededParam_ = 5;
            msb_ = 255;
            break;
        case 0x55:  // ESC U nnnn abc repeat hi-res dot column
            neededParam_ = 7;
            msb_ = 255;
            break;
        case 0x27:  // ESC '  select user-defined set
        case 0x49:  // ESC I  define user-defined characters
            // Not supported by the reference either. neededParam_ must be
            // cleared here: leaving the previous command's count armed made
            // phase 2 swallow the next 1-6 printable bytes as parameters.
            neededParam_ = 0;
            escCmd_ = 0;
            return true;
        default:
            neededParam_ = 0;
            escCmd_      = 0;
            return true;
        }

        if (neededParam_ > 0) return true;
    }

    // ── Phase 2: accumulate parameters ──────────────────────────────────
    if (numParam_ < neededParam_) {
        params_[numParam_++] = ch;
        if (numParam_ < neededParam_) return true;
    }
    if (escCmd_ == 0) {
        // ── Phase 3: bare control codes ─────────────────────────────────
        switch (ch) {
        case 0x00: return true;                 // NUL ignored
        case 0x07: return true;                 // BEL
        case 0x08: {                            // BS
            const double step = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;
            if (curX_ - step >= leftMargin_) curX_ -= step;
            return true;
        }
        case 0x09: {                            // HT
            // NEAREST stop to the right, not the farthest. The reference
            // (imagewriter.cpp:1131-1141) keeps overwriting `moveTo` as it
            // scans, so it lands on the LAST stop past the head — with
            // stops at 10/20/30 chars the first TAB jumped to 30 and every
            // later TAB was a no-op, which turns any columnar report into
            // one ragged column. A deliberate deviation from the
            // reference, matching the ImageWriter II Technical Reference.
            double moveTo = -1.0;
            for (uint8_t i = 0; i < numHorizTabs_; ++i)
                if (horiztabs_[i] > curX_ &&
                    (moveTo < 0.0 || horiztabs_[i] < moveTo))
                    moveTo = horiztabs_[i];
            if (moveTo > 0.0 && moveTo < rightMargin_) curX_ = moveTo;
            return true;
        }
        case 0x0b:                              // VT
            if (numVertTabs_ == 0) {
                curX_ = leftMargin_;            // all tabs cancelled → CR
            } else {
                double moveTo = -1.0;              // nearest stop below —
                for (uint8_t i = 0; i < numVertTabs_; ++i)   // see HT above
                    if (verttabs_[i] > curY_ &&
                        (moveTo < 0.0 || verttabs_[i] < moveTo))
                        moveTo = verttabs_[i];
                if (moveTo > bottomMargin_ - lineSpacing_ || moveTo < 0.0)
                    newPage(true, false);
                else
                    curY_ = moveTo;
            }
            return true;
        case 0x0c:                              // FF
            newPage(true, true);
            return true;
        case 0x0d:                              // CR
            curX_ = leftMargin_;
            if (switcha_ & kSwitchALfAfterCr) lineFeed();
            if (!autoFeedActive()) return true;
            lineFeed();
            crJustFed_ = true;    // an LF right after this one is the
            return true;          // guest's own — see the LF case
        case 0x0a:                              // LF
            if (wasCrFed) {
                // CR+LF from the guest: it manages its own line feeds, so
                // the feed the CR just did was ours to give and this LF
                // would double-space. Swallow it — and in Auto mode stop
                // feeding on CR at all from here on, which is what lets a
                // colour driver overprint its passes (Print Shop puts a
                // bare CR between its yellow/cyan/magenta passes).
                if (feedMode_ == AutoFeed::Auto && !feedLatchedOff_) {
                    feedLatchedOff_ = true;
                    if (trace_)
                        traceEvent("auto line-feed OFF — the guest sent its "
                                   "own LF after a CR");
                }
                return true;
            }
            lineFeed();
            return true;
        case 0x0e:                              // SO  double width on
            style_ |= kStyleDoubleWidth;  updateMetrics(); return true;
        case 0x0f:                              // SI  double width off
            style_ &= ~kStyleDoubleWidth; updateMetrics(); return true;
        case 0x11: return true;                 // DC1 select printer
        case 0x12:                              // DC2 cancel condensed
            hmi_ = -1.0;
            style_ &= ~kStyleCondensed;   updateMetrics(); return true;
        case 0x13: return true;                 // DC3 deselect printer
        case 0x14: return true;                 // DC4
        case 0x18: return true;                 // CAN
        case 0x1b: escSeen_ = true;  return true;   // ESC
        case 0x1f: fsSeen_  = true;  return true;   // US
        default:   return false;                // printable
        }
    }

    // ── Phase 4: execute the completed command ──────────────────────────
    if (trace_) traceCommand();
    // Several commands take their parameters as ASCII digit strings with
    // leading spaces; normalise those to '0' the way the reference does.
    // Unlike the reference (which blanket-converts params[0..3]) the count
    // is the number of *digit* positions, so `ESC R nnn ' '` still repeats
    // a space instead of printing zeros.
    auto spacesToZeros = [&](int digits) {
        for (int i = 0; i < digits && i < 20; ++i)
            if (params_[i] == ' ') params_[i] = '0';
    };
    auto param3 = [&]() {
        return paramDigit(params_[0]) * 100 + paramDigit(params_[1]) * 10 +
               paramDigit(params_[2]);
    };
    auto param4 = [&]() {
        return paramDigit(params_[0]) * 1000 + paramDigit(params_[1]) * 100 +
               paramDigit(params_[2]) * 10 + paramDigit(params_[3]);
    };

    switch (escCmd_) {
    case 0x73:                                  // ESC s n  intercharacter space
        if (style_ & kStyleProp) {
            extraIntraSpace_ = paramDigit(params_[0]) / 120.0;
            updateMetrics();
        }
        break;

    case 0x46: {                                // ESC F nnnn  absolute X
        spacesToZeros(4);
        const double unit = definedUnit_ < 0 ? 72.0 : definedUnit_;
        const double newX = leftMargin_ + param4() / unit;
        if (newX <= rightMargin_) curX_ = newX;
        break;
    }
    case 0x68: {                                // ESC h nnnn  absolute hi-res X
        spacesToZeros(4);
        const double unit = definedUnit_ < 0 ? 72.0 : definedUnit_ * 2.0;
        const double newX = leftMargin_ + param4() / unit;
        if (newX <= rightMargin_) curX_ = newX;
        break;
    }

    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: {
        // ESC 1..6 — add n/120" of intercharacter space (proportional
        // only), the same knob `ESC s n` sets. The reference
        // (imagewriter.cpp:665-678) assigns `curX_ = n/unit` instead: an
        // ABSOLUTE position a fraction of an inch from the left edge, so
        // `ESC 3` mid-line threw the head from 1.25" back to 0.02" —
        // outside the left margin — and destroyed every justified line a
        // proportional driver (AppleWorks, the LQ GS/OS driver) produced.
        // A deliberate deviation from the reference, matching the
        // ImageWriter II Technical Reference.
        if (style_ & kStyleProp) {
            extraIntraSpace_ = (escCmd_ - '0') / 120.0;
            updateMetrics();
        }
        break;
    }

    case 0x47: case 0x53:                       // ESC G/S nnnn  graphics
        spacesToZeros(4);
        printRes_ &= ~8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        break;
    case 0x43:                                  // ESC C nnnn  hi-res graphics
        spacesToZeros(4);
        printRes_ |= 8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        break;
    case 0x67:                                  // ESC g nnn  graphics (*8)
        spacesToZeros(3);
        printRes_ &= ~8;
        setupBitImage(printRes_, static_cast<uint32_t>(param3()) * 8u);
        break;

    case 0x56: {                                // ESC V nnnn c  repeat column
        spacesToZeros(4);
        printRes_ &= ~8;
        const int n = param4();
        for (int i = 0; i < n; ++i) {
            setupBitImage(printRes_, 1);
            printBitGraph(params_[4]);
        }
        msb_ = 0;
        break;
    }
    case 0x55: {                                // ESC U nnnn abc  hi-res repeat
        spacesToZeros(4);
        printRes_ |= 8;
        const int n = param4();
        for (int i = 0; i < n; ++i) {
            setupBitImage(printRes_, 1);
            printBitGraph(params_[4]);
            printBitGraph(params_[5]);
            printBitGraph(params_[6]);
        }
        msb_ = 0;
        break;
    }

    case 0x74:                                  // ESC t n  shift down n/216
        verticalDot_ = static_cast<uint8_t>(paramDigit(params_[0]));
        break;

    // Pitch selection — each also fixes the graphics density and the unit
    // used by ESC F / ESC h (imagewriter.cpp:765-836).
    case 0x6e: cpi_ =  9.0; printRes_ = 0; definedUnit_ =  72;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x4e: cpi_ = 10.0; printRes_ = 1; definedUnit_ =  80;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x45: cpi_ = 12.0; printRes_ = 2; definedUnit_ =  96;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x65: cpi_ = 13.4; printRes_ = 3; definedUnit_ = 107;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x71: cpi_ = 15.0; printRes_ = 4; definedUnit_ = 120;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x51: cpi_ = 17.0; printRes_ = 5; definedUnit_ = 136;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x70: cpi_ = 10.0; printRes_ = 6; definedUnit_ = 144;
               style_ |= kStyleProp;  updateMetrics(); break;
    case 0x50: cpi_ = 12.0; printRes_ = 7; definedUnit_ = 160;
               style_ |= kStyleProp;  updateMetrics(); break;

    case 0x54:                                  // ESC T nn  nn/144 in spacing
        lineSpacing_ = (paramDigit(params_[0]) * 10 +
                        paramDigit(params_[1])) / 144.0;
        break;
    case 0x42: lineSpacing_ = 1.0 / 8.0; break; // ESC B
    case 0x41: lineSpacing_ = 1.0 / 6.0; break; // ESC A

    case 0x58: style_ |= kStyleUnderline;  break;   // ESC X
    case 0x59: style_ &= ~kStyleUnderline; break;   // ESC Y

    case 0x3c: case 0x3e: break;                // uni/bidirectional — no head
    case 0x63: resetPrinter(); break;           // ESC c  initialize

    case 0x48:                                  // ESC H nnnn  page length
        spacesToZeros(4);
        // Clamped to something a sheet can be. `ESC H 0000` set a
        // zero-length page, so every single line feed ejected: three LFs
        // produced three sheets and a real job blew the whole 32-page
        // stack away in blanks. The upper end is the physical raster —
        // past it the head runs off the bottom and the ink is dropped
        // silently, with no eject to show for it.
        pageHeightIn_ = std::clamp(param4() / 144.0,
                                   1.0, defaultPageHeight_);
        bottomMargin_ = pageHeightIn_;
        topMargin_    = 0.0;
        updateSwitch();
        break;

    case 0x21: style_ |= kStyleBold;  updateMetrics(); break;   // ESC !
    case 0x22: style_ &= ~kStyleBold; updateMetrics(); break;   // ESC "
    case 0x78: style_ |= kStyleSuperscript; break;              // ESC x
    case 0x79: style_ |= kStyleSubscript;   break;              // ESC y
    case 0x7a: style_ &= ~(kStyleSuperscript | kStyleSubscript); break; // ESC z
    case 0x77: style_ |= kStyleHalfHeight;  break;              // ESC w
    case 0x57: style_ &= ~kStyleHalfHeight; break;              // ESC W

    case 0x72: if (lineSpacing_ > 0) lineSpacing_ *= -1; break; // ESC r reverse
    case 0x66: if (lineSpacing_ < 0) lineSpacing_ *= -1; break; // ESC f forward

    case 0x61: case 0x6d: case 0x4d: break;     // typeface select — one font
    case 0x3d: break;                           // internal font ID
    case 0x24: break;                           // cancel MSB + MouseText
    case 0x3f: break;                           // ESC ?  ID string: no back-channel
    case 0x4f: case 0x6f: break;                // paper-out detector
    case 0x6b: break;                           // optional font
    case 0x6c: break;                           // CR before LF/FF
    case 0x40: break;                           // output bin
    case 0x2b: case 0x2e: break;                // custom char width

    case 0x4c:                                  // ESC L nnn  left margin
        spacesToZeros(3);
        // Clamped to the sheet. `ESC L 000` gave a NEGATIVE margin (the
        // -1 is the 1-based column) and clipped the first character;
        // `ESC L 999` put it 83" out and every page came out blank, with
        // no diagnostic either way. A margin off the paper is a garbled
        // parameter, not an instruction.
        leftMargin_ = std::clamp((param3() - 1.0) / cpi_,
                                 0.0, std::max(0.0, pageWidthIn_ - 1.0 / cpi_));
        if (curX_ < leftMargin_) curX_ = leftMargin_;
        break;

    case 0x4b: {                                // ESC K n  ribbon colour
        const int n = paramDigit(params_[0]);
        // A black cartridge has one band: the printer takes the command
        // and prints black anyway, like the real thing.
        if (n >= 0 && n <= 6)
            color_ = static_cast<uint8_t>(
                (ribbon_ == Ribbon::Black ? 7 : kRibbonBand[n]) << 5);
        break;
    }

    case 0x52: {                                // ESC R nnn c  repeat char
        spacesToZeros(3);
        const int n = param3();
        const uint8_t c = params_[3];
        escCmd_ = 0;                            // so the recursion prints
        for (int i = 0; i < n; ++i) printCharInternal(c);
        break;
    }

    case 0x30: numHorizTabs_ = 0; break;        // ESC 0  clear all tabs

    case 0x28: {                                // ESC ( nnn,  set tabs
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        if (params_[3] == ',' && numHorizTabs_ < 32) {
            horiztabs_[numHorizTabs_++] = stop;
            numParam_    = 0;
            neededParam_ = 4;                   // another stop follows
            return true;
        }
        if (numHorizTabs_ < 32) horiztabs_[numHorizTabs_++] = stop;
        break;
    }
    case 0x29: {                                // ESC ) nnn,  delete tabs
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        for (uint8_t i = 0; i < numHorizTabs_; ++i)
            if (horiztabs_[i] == stop) horiztabs_[i] = 0.0;
        if (params_[3] == ',') {
            numParam_    = 0;
            neededParam_ = 4;
            return true;
        }
        break;
    }
    case 0x75: {                                // ESC u nnn  add one tab stop
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        bool haveStop = false;
        int  lastEmpty = (numHorizTabs_ == 32) ? 33 : numHorizTabs_;
        for (uint8_t i = 0; i < numHorizTabs_; ++i) {
            if (horiztabs_[i] == stop)  haveStop  = true;
            if (horiztabs_[i] == 0.0)   lastEmpty = i;
        }
        if (!haveStop && lastEmpty < 33) {
            horiztabs_[lastEmpty] = stop;
            if (lastEmpty == numHorizTabs_) ++numHorizTabs_;
        }
        break;
    }

    case 0x5a:                                  // ESC Z nn  open switches
        switcha_ &= ~params_[0];
        switchb_ &= ~params_[1];
        updateSwitch();
        break;
    case 0x44:                                  // ESC D nn  close switches
        switcha_ |= params_[0];
        switchb_ |= params_[1];
        updateSwitch();
        break;

    case 0x833: {                               // US n  feed n blank lines
        const int n = paramDigit(params_[0]);
        for (int i = 0; i < n; ++i) lineFeed();
        break;
    }

    default:
        break;
    }

    escCmd_ = 0;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Palette
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::indexToRgb(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    // FillPalette (imagewriter.cpp:101-114) in closed form: each ribbon
    // band names the channels the ink *subtracts*, and 30.9 is the
    // reference's divisor for the 0..31 intensity ramp.
    const uint8_t band = static_cast<uint8_t>(v >> 5);
    const float   i    = static_cast<float>(v & 0x1F);

    // band bit 0 = magenta ink (kills green), bit 1 = cyan (kills red),
    // bit 2 = yellow (kills blue).
    const float redMax   = (band & 2) ? 255.0f : 0.0f;
    const float greenMax = (band & 1) ? 255.0f : 0.0f;
    const float blueMax  = (band & 4) ? 255.0f : 0.0f;

    auto ch = [&](float maxv) {
        const float f = 255.0f - (maxv / 30.9f) * i;
        return static_cast<uint8_t>(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
    };
    r = ch(redMax);
    g = ch(greenMax);
    b = ch(blueMax);
}

void ImageWriter::pageToRgba(const Page& p, std::vector<uint8_t>& out)
{
    out.resize(static_cast<size_t>(p.w) * p.h * 4);
    // 256-entry LUT — the page is megapixels, the palette is not.
    uint8_t lut[256][3];
    for (int i = 0; i < 256; ++i)
        indexToRgb(static_cast<uint8_t>(i), lut[i][0], lut[i][1], lut[i][2]);

    for (size_t i = 0, n = p.pix.size(); i < n; ++i) {
        const uint8_t* c = lut[p.pix[i]];
        out[i * 4 + 0] = c[0];
        out[i * 4 + 1] = c[1];
        out[i * 4 + 2] = c[2];
        out[i * 4 + 3] = 255;
    }
}

// ─────────────────────────────────────────────────────────────────────────

ImageWriter::Status ImageWriter::status() const
{
    static constexpr uint16_t kHoriz[8] = { 72, 80, 96, 107, 120, 136, 144, 160 };

    Status s;
    s.headX       = curX_;
    s.headY       = curY_;
    s.cpi         = actcpi_;
    s.lineSpacing = lineSpacing_;
    s.colorName   = bandName(static_cast<uint8_t>(color_ >> 5));
    s.graphicsDpi = kHoriz[printRes_ & 7];
    s.inGraphics  = bitGraph_.remBytes > 0;

    struct { uint16_t bit; const char* name; } kNames[] = {
        { kStyleProp,        "proportional" },
        { kStyleCondensed,   "condensed"    },
        { kStyleBold,        "bold"         },
        { kStyleDoubleWidth, "double-width" },
        { kStyleItalics,     "italic"       },
        { kStyleUnderline,   "underline"    },
        { kStyleSuperscript, "superscript"  },
        { kStyleSubscript,   "subscript"    },
        { kStyleHalfHeight,  "half-height"  },
    };
    for (const auto& n : kNames) {
        if (!(style_ & n.bit)) continue;
        if (!s.styleText.empty()) s.styleText += ' ';
        s.styleText += n.name;
    }
    if (s.styleText.empty()) s.styleText = "normal";
    return s;
}

} // namespace pom2
