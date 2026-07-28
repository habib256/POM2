// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriter — host-side Apple ImageWriter II / LQ printer.
//
// This is the *printer*, not the interface card. The Apple II talks to a
// printer through a card (`PrinterCard`, `GrapplerCard`, later an SSC);
// every byte those cards spool is fed here, and this class interprets the
// ImageWriter control language and paints dots onto a page raster that the
// UI shows in a window (`ImageWriter_ImGui`) and saves as PNG.
//
// Source of truth
// ---------------
// Ported from greg-kennedy/ImageWriter (`imagewriter.cpp`, the GSport /
// KEGS / DOSBox lineage by Christopher G. Mason), itself written against
// Apple's "ImageWriter II Technical Reference Manual" (ISBN 0-201-17766-8)
// and "ImageWriter LQ Reference Manual" (ISBN 0-201-17751-X). Command
// dispatch, the soft-switch model, the density tables and the subtractive
// colour-ribbon encoding are line-for-line equivalents; the cited line
// ranges appear on each ported block below.
//
// Three deliberate deviations from the reference, all forced by POM2's
// "no new third-party dependency" rule (the reference needs SDL 1.2 +
// FreeType, neither of which POM2 links):
//
//   1. Glyphs come from the repo's own 8x8 CP437 dot-matrix font
//      (`hgrpaint::kBBFontCp437`, 7 px wide + 1 px gap) instead of a
//      FreeType-rendered TrueType face. This is *closer* to the real
//      hardware, not further: an ImageWriter draft character really is an
//      8-dot-wide cell at the pitch's graphics density, so a character and
//      a graphics column go through the same dot plotter here.
//   2. Dots are painted as the page-pixel interval they cover
//      (`fillDots`) instead of the reference's `pixsize` heuristic with its
//      "Primative scaling function" fudge (imagewriter.cpp:1556-1573).
//      Adjacent dots abut exactly at any page DPI, so graphics dumps have
//      no seams and no double-width columns.
//   3. `resetPrinter()` leaves bold OFF. The reference switches it on
//      (imagewriter.cpp:289) to thicken a thin TrueType face; on a real
//      dot-matrix cell that just smears every glyph.
//
// Proportional mode (ESC p / ESC P) selects the stated pitch but keeps the
// fixed 7-dot cell — the font has no per-glyph advance table.
//
// Page raster encoding (verbatim from the reference, imagewriter.cpp:203-208)
// -------------------------------------------------------------------------
// One byte per page pixel, `yyyxxxxx`: the low 5 bits are ink intensity
// (31 = full) and the top 3 bits are the colour-ribbon band. The bands are
// chosen so that OR-ing two inks mixes them subtractively the way overprint
// on a real four-band ribbon does:
//
//     001 magenta   010 cyan     100 yellow
//     011 blue      101 red      110 green      111 black
//
// so magenta|yellow = 101 = red, cyan|yellow = 110 = green, and all three
// = 111 = black. Index 0 is blank paper (white).
//
// Threading
// ---------
// UI-thread only. The emulator core never touches this object; the UI
// drains the interface card's spool (which *is* mutex-guarded) and feeds
// the bytes here between frames.

#ifndef POM2_IMAGEWRITER_H
#define POM2_IMAGEWRITER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pom2 {

class ImageWriter
{
public:
    // ─── Style bits (imagewriter.h:74-83) ────────────────────────────────
    enum : uint16_t {
        kStyleProp         = 0x001,
        kStyleCondensed    = 0x002,
        kStyleBold         = 0x004,
        kStyleDoubleStrike = 0x008,
        kStyleDoubleWidth  = 0x010,
        kStyleItalics      = 0x020,
        kStyleUnderline    = 0x040,
        kStyleSuperscript  = 0x080,
        kStyleSubscript    = 0x100,
        kStyleHalfHeight   = 0x200,
    };

    /// Paper sizes offered by the GS/OS ImageWriter LQ driver, in
    /// PostScript points (iw_charmaps.h:62-77).
    enum class PaperSize : uint8_t {
        Letter = 0,     // 8.5 x 11 in
        Legal,          // 8.5 x 14 in
        A4,             // 210 x 297 mm
        B5,             // 176 x 250 mm
        WideFanfold,    // 14 x 11 in
        Ledger,         // 11 x 17 in
        A3,             // 297 x 420 mm
        Count
    };
    static const char* paperSizeName(PaperSize s);

    /// A rendered sheet. `pix` is `w*h` bytes in the `yyyxxxxx` encoding
    /// described in the file header. `dpi` is the raster density the sheet
    /// was printed at — completed sheets keep theirs even if the host
    /// changes the printer DPI afterwards, so an exporter can recover the
    /// physical page size (PDF media box) from `w/dpi` x `h/dpi` inches.
    struct Page {
        int                  w = 0;
        int                  h = 0;
        std::vector<uint8_t> pix;
        int                  dpi = 144;   // = kDefaultDpi (declared below)
    };

    /// Front-panel / print-head readout for the UI status line.
    struct Status {
        double      headX      = 0.0;   // inches from the left paper edge
        double      headY      = 0.0;   // inches from the top paper edge
        double      cpi        = 0.0;   // effective characters per inch
        double      lineSpacing= 0.0;   // inches
        const char* colorName  = "";    // current ribbon band
        std::string styleText;          // "bold underline …" ("normal" if none)
        int         graphicsDpi = 0;    // horizontal density of the active pitch
        bool        inGraphics  = false;// mid bit-image (remBytes > 0)
    };

    static constexpr int    kDefaultDpi = 144;
    static constexpr int    kMinDpi     = 72;
    static constexpr int    kMaxDpi     = 300;
    /// Completed sheets kept in RAM. A Letter page at 144 dpi is ~1.9 MB
    /// indexed, so 32 sheets is ~62 MB worst case; older sheets are
    /// dropped (counted by `droppedPageCount()`) rather than growing
    /// without bound when a guest form-feeds in a loop.
    static constexpr size_t kMaxPages   = 32;

    explicit ImageWriter(int dpi = kDefaultDpi,
                         PaperSize paper = PaperSize::Letter);

    /// Closes the trace if one is open. Without this a quit while tracing
    /// left the last partial hex row stranded in `traceRow_` (it never
    /// reached stdio, so the C runtime's exit flush could not save it) and
    /// the file ended with no `# trace closed` footer — you could not tell
    /// a complete trace from one cut short by a crash. That matters most
    /// on the `POM2_TRACE_PRINTER=1` path, which is the one used to
    /// capture a trace for a bug report.
    ~ImageWriter();

    /// Owns a `FILE*`; copying it would `fclose` the same handle twice.
    ImageWriter(const ImageWriter&)            = delete;
    ImageWriter& operator=(const ImageWriter&) = delete;

    // ─── Configuration (host side — not driven by the guest) ─────────────
    int       dpi()       const { return dpi_; }
    PaperSize paperSize() const { return paper_; }
    /// Both rebuild the page raster, so the sheet in progress is lost.
    /// Completed sheets are kept (they carry their own size).
    void setDpi(int dpi);
    void setPaperSize(PaperSize s);

    /// The ImageWriter's "line feed after carriage return" DIP switch
    /// (SW A-8) — the setting that decides whether a printout comes out
    /// right, double-spaced, or overprinted onto one line, and the one no
    /// user should have to guess.
    ///
    ///   * A bare Apple II `PR#n : PRINT` emits CR and never LF, so the
    ///     printer has to supply the feed.
    ///   * Real drivers (and the Grappler+ firmware) send CR **and** LF,
    ///     so the printer must NOT add one, or everything double-spaces.
    ///   * Colour drivers go further: Print Shop separates its yellow /
    ///     cyan / magenta passes with a bare CR precisely so they
    ///     overprint the same line. Feed there and the passes stagger
    ///     down the page in a coloured staircase.
    ///
    /// `Auto` (the default) settles it from the stream itself: it feeds
    /// on CR until it sees the guest send its own LF right after one, and
    /// then stops for the rest of the job. All three cases come out
    /// right with nothing to configure. `On` / `Off` pin the switch.
    enum class AutoFeed : uint8_t { Auto = 0, On, Off, Count };

    /// Which ribbon cartridge is fitted. There is no "colour mode" on an
    /// ImageWriter II: colour is a *four-band ribbon* you physically
    /// install, and the guest picks a band with `ESC K n`. With the plain
    /// black cartridge the printer still accepts ESC K — it just has one
    /// band, so everything comes out black. Modelled the same way here.
    ///
    /// Note this only decides what the ribbon *can* do. Software has to
    /// ask: Print Shop, for one, only emits ESC K when its Setup names an
    /// "Apple Imagewriter II **(C)**" — the (M) driver never sends colour
    /// no matter what is in the printer.
    enum class Ribbon : uint8_t { FourColour = 0, Black, Count };
    static const char* ribbonName(Ribbon r);
    void   setRibbon(Ribbon r) { ribbon_ = r; }
    Ribbon ribbon() const { return ribbon_; }

    static const char* autoFeedName(AutoFeed m);
    void     setAutoFeedMode(AutoFeed m);
    AutoFeed autoFeedMode() const { return feedMode_; }
    /// What the mechanism is actually doing right now (Auto resolved).
    bool     autoFeedActive() const {
        return feedMode_ == AutoFeed::On ||
               (feedMode_ == AutoFeed::Auto && !feedLatchedOff_);
    }
    /// True once Auto has seen the guest manage its own line feeds.
    bool     autoFeedLatchedOff() const { return feedLatchedOff_; }

    // Back-compat shims (tests + old settings): On/Off only.
    void setAutoFeed(bool on) {
        setAutoFeedMode(on ? AutoFeed::On : AutoFeed::Off);
    }
    bool autoFeed() const { return autoFeedActive(); }

    // ─── Data path ───────────────────────────────────────────────────────
    void printChar(uint8_t ch);
    void printBytes(const uint8_t* data, size_t n);

    // ─── Mechanism pacing ────────────────────────────────────────────────
    // The interface card hands bytes over at bus speed — a `PRINT` loop
    // spools a whole page in a millisecond of emulated time. A real
    // ImageWriter II eats them at the speed of its carriage and platen,
    // so `queueBytes` + `tick` model the mechanism: bytes wait in the
    // printer's input buffer and are printed as the head can reach them.
    //
    // Speeds are Apple's published figures (ImageWriter II Owner's
    // Manual, "Specifications"): 250 cps draft, 45 cps near-letter-
    // quality. `Instant` is the old behaviour — everything prints the
    // frame it arrives.
    enum class Speed : uint8_t { Instant = 0, Draft, NLQ, Count };
    static const char* speedName(Speed s);

    void  setSpeed(Speed s);
    Speed speed() const { return speed_; }

    /// Hand `n` bytes to the printer's input buffer. They print over the
    /// next ticks; nothing is rendered here.
    void queueBytes(const uint8_t* data, size_t n);
    /// Advance the mechanism by `dt` seconds of host time, printing
    /// whatever the head had time to lay down.
    void tick(double dt);
    /// Print everything still queued right now ("Print now" button).
    void flushPending();

    /// Bytes accepted but not yet printed.
    size_t pendingBytes() const { return pending_.size() - pendingHead_; }
    bool   busy() const { return pendingHead_ < pending_.size(); }

    /// Stock ImageWriter II input buffer. The interface card raises BUSY
    /// to the Apple II while more than this is outstanding, which is what
    /// makes a printing guest actually wait for the printer.
    static constexpr size_t kInputBufferBytes = 2048;

    // ─── Trace log ───────────────────────────────────────────────────────
    // A printout that comes out as noise is a *protocol* problem: the
    // guest's driver and this printer disagree about the command set. The
    // only way to see that is the byte stream itself, decoded. The trace
    // writes every byte the mechanism consumes as a hex dump interleaved
    // with the decoded command, every page event, and every host event the
    // caller reports through `traceEvent` (BUSY, queue depth).
    //
    // Enable from the panel, or set `POM2_TRACE_PRINTER=1` before launch
    // (`POM2_TRACE_PRINTER=<path>` to choose the file).
    bool startTrace(const std::string& path, std::string& err);
    void stopTrace();
    bool tracing() const { return trace_ != nullptr; }
    const std::string& tracePath() const { return tracePath_; }
    /// Log a host-side line (printf-style). No-op when not tracing.
    void traceEvent(const char* fmt, ...);

    /// Rolling capture of the raw bytes the printer received, always on
    /// (last `kRawCaptureBytes`). The trace has to be armed *before* the
    /// job; this doesn't — so a printout that came out wrong can still be
    /// handed over byte-for-byte afterwards ("Save raw stream" in the
    /// panel). Cheap: an append to a capped vector per byte.
    static constexpr size_t kRawCaptureBytes = 256 * 1024;
    const std::vector<uint8_t>& rawStream() const { return raw_; }
    void clearRawStream() { raw_.clear(); }

    // ─── Front panel ─────────────────────────────────────────────────────
    /// FORM FEED button: eject the sheet (ignored when it is still blank).
    void formFeed();
    /// Power cycle: every setting back to factory defaults, sheet cleared.
    void resetPrinterHard();
    /// Power cycle *and* throw away the completed-sheet stack.
    void clearAll();

    // ─── Raster access ───────────────────────────────────────────────────
    int          pageWidth()  const { return current_.w; }
    int          pageHeight() const { return current_.h; }
    const Page&  currentPage() const { return current_; }
    bool         currentPageBlank() const;
    size_t       completedPageCount() const { return pages_.size(); }
    const Page&  completedPage(size_t idx) const { return pages_[idx]; }
    size_t       droppedPageCount() const { return droppedPages_; }

    /// Expand an indexed page into top-down RGBA8 (4 bytes/pixel).
    static void pageToRgba(const Page& p, std::vector<uint8_t>& out);
    /// Palette entry for one indexed byte (see the file header encoding).
    static void indexToRgb(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);

    // ─── Odometer / UI helpers ───────────────────────────────────────────
    uint64_t bytesReceived() const { return bytesIn_; }
    /// Bumped on every change to the current raster, so the viewer can
    /// skip re-uploading an unchanged texture.
    uint32_t revision() const { return revision_; }
    Status   status() const;

private:
    // ─── Page geometry ───────────────────────────────────────────────────
    int       dpi_;
    PaperSize paper_;
    double    defaultPageWidth_  = 8.5;   // inches
    double    defaultPageHeight_ = 11.0;

    Page              current_;
    std::vector<Page> pages_;
    size_t            droppedPages_ = 0;
    uint32_t          revision_     = 1;
    uint64_t          bytesIn_      = 0;

    // ─── Printer state (imagewriter.h:214-294) ───────────────────────────
    uint8_t  color_ = 7 << 5;             // COLOR_BLACK
    uint8_t  switcha_ = 0, switchb_ = ' ';
    uint8_t  msb_ = 0;                    // 255 = pass bit 7 through

    double   curX_ = 0.0, curY_ = 0.0;    // print head, inches

    uint16_t escCmd_  = 0;
    bool     escSeen_ = false, fsSeen_ = false;
    uint8_t  numParam_ = 0, neededParam_ = 0;
    uint8_t  params_[20]{};

    uint16_t style_ = 0;
    double   cpi_ = 12.0, actcpi_ = 12.0;
    uint8_t  verticalDot_ = 0;

    double   topMargin_ = 0.0, bottomMargin_ = 11.0;
    double   leftMargin_ = 0.25, rightMargin_ = 8.5;
    double   pageWidthIn_ = 8.5, pageHeightIn_ = 11.0;
    double   lineSpacing_ = 1.0 / 6.0;

    double   horiztabs_[32]{};
    uint8_t  numHorizTabs_ = 0;
    double   verttabs_[16]{};
    uint8_t  numVertTabs_ = 0;

    uint8_t  printRes_ = 2;               // pitch → graphics density index
    double   extraIntraSpace_ = 0.0;
    double   definedUnit_ = 96.0;
    double   hmi_ = -1.0;                 // horizontal motion index override

    Ribbon   ribbon_ = Ribbon::FourColour;   // see setRibbon()

    // ─── Line-feed-after-CR switch (see setAutoFeedMode) ─────────────────
    AutoFeed feedMode_       = AutoFeed::Auto;
    bool     feedLatchedOff_ = false;   // Auto saw the guest's own LF
    bool     crJustFed_      = false;   // last byte was a CR that fed

    // ─── Mechanism pacing (see queueBytes/tick) ──────────────────────────
    Speed                speed_ = Speed::Draft;
    std::vector<uint8_t> pending_;
    size_t               pendingHead_ = 0;
    double               credit_ = 0.0;   // banked mechanism seconds

    /// Seconds the mechanism needs to consume `ch` in the current state
    /// (glyph, dot column, carriage return, paper feed, or a free
    /// command byte). 0 in `Instant` mode.
    double byteCost(uint8_t ch) const;

    /// How long the byte at the head of the queue has gone unaffordable.
    /// A cost model that can never afford a byte would wedge the printer
    /// *and* the guest waiting on it (that is exactly what a form feed
    /// under a too-low credit cap did), so past `kStallSeconds` the byte
    /// is forced through and the trace says so.
    double stalledFor_ = 0.0;
    static constexpr double kStallSeconds = 10.0;

    // ─── Trace log ───────────────────────────────────────────────────────
    std::vector<uint8_t> raw_;         // see rawStream()
    std::FILE*  trace_       = nullptr;
    std::string tracePath_;
    double      traceClock_  = 0.0;    // seconds of tick() time, for stamps
    uint8_t     traceRow_[16]{};
    int         traceRowLen_ = 0;
    uint64_t    traceOffset_ = 0;
    void traceByte(uint8_t ch);
    void traceFlushRow();
    void traceCommand();

    /// Bit-image state (imagewriter.h:254-262).
    struct BitGraph {
        uint16_t horizDens = 72, vertDens = 72;
        bool     adjacent  = true;
        uint8_t  bytesColumn = 1;
        uint32_t remBytes  = 0;
        uint8_t  column[6]{};
        uint8_t  readBytesColumn = 0;
    } bitGraph_;

    /// ASCII → CP437 glyph index. Identity except for the ten positions
    /// the international soft-switches (A-1..A-3) remap.
    uint8_t  curMap_[256]{};

    // ─── Internals ───────────────────────────────────────────────────────
    void resetPrinter();
    void rebuildPage();
    void newPage(bool save, bool resetx);
    void updateSwitch();
    void updateMetrics();
    void selectDefaultMap();

    /// printChar() minus the byte odometer — the ESC R repeat handler
    /// re-enters here so a 200-character run counts as the 5 bytes the
    /// interface card actually delivered.
    void printCharInternal(uint8_t ch);

    bool processCommandChar(uint8_t ch);
    void renderGlyph(uint8_t ch);
    void setupBitImage(uint8_t dens, uint32_t numCols);
    void printBitGraph(uint8_t ch);

    /// Paint the page-pixel rectangle covered by [x, x+w) x [y, y+h)
    /// inches with full-intensity ink of the current ribbon colour.
    void fillDots(double xInch, double yInch, double wInch, double hInch);

    void lineFeed();
};

} // namespace pom2

#endif // POM2_IMAGEWRITER_H
