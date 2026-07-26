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
    /// described in the file header.
    struct Page {
        int                  w = 0;
        int                  h = 0;
        std::vector<uint8_t> pix;
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

    // ─── Configuration (host side — not driven by the guest) ─────────────
    int       dpi()       const { return dpi_; }
    PaperSize paperSize() const { return paper_; }
    /// Both rebuild the page raster, so the sheet in progress is lost.
    /// Completed sheets are kept (they carry their own size).
    void setDpi(int dpi);
    void setPaperSize(PaperSize s);

    /// The ImageWriter's "line feed after carriage return" DIP switch
    /// (SW A-8). Defaults ON because the Apple II's COUT emits a bare CR
    /// ($8D) and never an LF — with it off every printout lands on one
    /// overprinted line. Turn it off for drivers that send CR+LF
    /// themselves (they double-space otherwise).
    void setAutoFeed(bool on) { autoFeed_ = on; }
    bool autoFeed() const     { return autoFeed_; }

    // ─── Data path ───────────────────────────────────────────────────────
    void printChar(uint8_t ch);
    void printBytes(const uint8_t* data, size_t n);

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

    bool     autoFeed_ = true;    // see setAutoFeed()

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
