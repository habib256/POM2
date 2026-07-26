// ImageWriter smoke test — pins the host-side Apple ImageWriter II printer
// (src/ImageWriter.cpp, ported from greg-kennedy/ImageWriter) plus the
// spool→printer seam the UI streams through.
//
// What is pinned, and why each matters:
//
//   1. Paper geometry — page raster = paper size (points/72) x page DPI.
//      Everything downstream (dot positions, PNG export) is in those units.
//   2. Text path — a glyph lays down ink; bit 7 is stripped so Apple II
//      output ("HELLO" with the high bit set, which is what COUT emits)
//      prints the same as plain ASCII; CR parks the head at the left
//      margin; LF advances by the line spacing.
//   3. Colour ribbon — ESC K selects a band, and overprinting two bands
//      ORs them into the correct mixed colour (magenta|yellow = red).
//      This is the encoding the whole page raster is built on.
//   4. Bit-image graphics — ESC G nnnn consumes exactly nnnn bytes as
//      dot columns and puts each column's 8 pins on paper at the pitch's
//      density. Screen dumps and Print Shop output are nothing but this.
//   5. Command framing — ESC R (repeat) expands without inflating the
//      byte odometer, and an unknown ESC command swallows only itself.
//   6. Paper handling — FF ejects onto the completed stack, a blank sheet
//      is not ejected by the FORM FEED button, and the stack is capped
//      (a guest that form-feeds in a loop must not exhaust host RAM).
//   7. Spool seam — PrinterCard::drainSpoolFrom hands over exactly the
//      bytes written since the previous poll, and replays from 0 after a
//      clearSpool() so the printer never goes silently deaf.

#include "ImageWriter.h"
#include "PrinterCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using pom2::ImageWriter;

namespace {

void feed(ImageWriter& iw, const char* s)
{
    iw.printBytes(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

size_t inkPixels(const ImageWriter::Page& p)
{
    size_t n = 0;
    for (uint8_t v : p.pix) if (v != 0) ++n;
    return n;
}

/// Ribbon band (top 3 bits) of the first inked pixel, or 0 if the page is
/// blank.
uint8_t firstBand(const ImageWriter::Page& p)
{
    for (uint8_t v : p.pix) if (v != 0) return static_cast<uint8_t>(v >> 5);
    return 0;
}

void testPaperGeometry()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    assert(iw.pageWidth()  == static_cast<int>(8.5 * 144));   // 1224
    assert(iw.pageHeight() == static_cast<int>(11.0 * 144));  // 1584
    assert(iw.currentPageBlank());
    assert(iw.completedPageCount() == 0);

    iw.setPaperSize(ImageWriter::PaperSize::A4);
    assert(iw.pageWidth()  == static_cast<int>(595 / 72.0 * 144));
    assert(iw.pageHeight() == static_cast<int>(842 / 72.0 * 144));

    iw.setDpi(72);
    assert(iw.dpi() == 72);
    assert(iw.pageWidth() == static_cast<int>(595 / 72.0 * 72));

    // Out-of-range DPI clamps rather than producing a degenerate raster.
    iw.setDpi(10000);
    assert(iw.dpi() == ImageWriter::kMaxDpi);

    std::printf("  ok: paper geometry (size x DPI → raster)\n");
}

void testTextAndHighBit()
{
    ImageWriter plain(144, ImageWriter::PaperSize::Letter);
    feed(plain, "HELLO");
    const size_t plainInk = inkPixels(plain.currentPage());
    assert(plainInk > 0);
    assert(firstBand(plain.currentPage()) == 7);        // black ribbon

    // Apple II COUT sets bit 7 on every character; soft switch B-6 is open
    // by default, so the printer must mask it back off.
    ImageWriter hiBit(144, ImageWriter::PaperSize::Letter);
    const uint8_t msbHello[] = { 'H'|0x80, 'E'|0x80, 'L'|0x80, 'L'|0x80, 'O'|0x80 };
    hiBit.printBytes(msbHello, sizeof(msbHello));
    assert(hiBit.currentPage().pix == plain.currentPage().pix);

    // The "LF after CR" DIP switch defaults on — the Apple II never sends
    // an LF, so with it off every printout would overprint one line.
    assert(plain.autoFeed());
    {
        ImageWriter cr(144, ImageWriter::PaperSize::Letter);
        const double before = cr.status().headY;
        feed(cr, "A\r");
        assert(cr.status().headY > before);
        cr.resetPrinterHard();
        cr.setAutoFeed(false);
        const double flat = cr.status().headY;
        feed(cr, "A\r");
        assert(cr.status().headY == flat);
    }

    // CR returns the head to the left margin, LF advances one line.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setAutoFeed(false);
    feed(iw, "AB");
    const double afterTwo = iw.status().headX;
    assert(afterTwo > 0.25);                            // moved right of margin
    feed(iw, "\r");
    assert(iw.status().headX == 0.25);                  // ImageWriter left margin
    const double y0 = iw.status().headY;
    feed(iw, "\n");
    const double y1 = iw.status().headY;
    assert(y1 > y0 && (y1 - y0) > 0.16 && (y1 - y0) < 0.17);   // 1/6 in

    // ESC B → 1/8 in spacing.
    feed(iw, "\x1b" "B\n");
    const double y2 = iw.status().headY;
    assert((y2 - y1) > 0.12 && (y2 - y1) < 0.13);

    std::printf("  ok: glyph ink, bit-7 strip, CR/LF, ESC A/B spacing\n");
}

void testColorRibbon()
{
    // ESC K 1 = yellow (band 4), ESC K 2 = magenta (band 1).
    // Auto-LF off so the CR below re-strikes the SAME line.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    iw.setAutoFeed(false);
    feed(iw, "\x1b" "K1M");
    assert(firstBand(iw.currentPage()) == 4);

    // Overprint magenta on the same spot: the bands OR into red (band 5).
    feed(iw, "\r\x1b" "K2M");
    bool sawRed = false;
    for (uint8_t v : iw.currentPage().pix)
        if ((v >> 5) == 5) { sawRed = true; break; }
    assert(sawRed);

    feed(iw, "\x1b" "K0");
    assert(std::string(iw.status().colorName) == "black");

    // Palette: blank paper is white, full-intensity black band is black,
    // and the yellow band subtracts only blue.
    uint8_t r = 0, g = 0, b = 0;
    ImageWriter::indexToRgb(0, r, g, b);
    assert(r == 255 && g == 255 && b == 255);
    ImageWriter::indexToRgb(static_cast<uint8_t>((7 << 5) | 0x1F), r, g, b);
    assert(r == 0 && g == 0 && b == 0);
    ImageWriter::indexToRgb(static_cast<uint8_t>((4 << 5) | 0x1F), r, g, b);
    assert(r == 255 && g == 255 && b == 0);

    std::printf("  ok: ESC K ribbon bands + subtractive overprint + palette\n");
}

void testBitImageGraphics()
{
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "E");                   // 12 cpi → 96 dpi graphics
    assert(iw.status().graphicsDpi == 96);

    feed(iw, "\x1b" "G0004");               // four dot columns follow
    assert(iw.status().inGraphics);
    for (int i = 0; i < 4; ++i) iw.printChar(0xFF);   // all 8 pins
    assert(!iw.status().inGraphics);

    // 4 columns x 8 pins, each dot covering 144/96 = 1.5 px horizontally
    // and 144/72 = 2 px vertically → a solid block, no gaps.
    const size_t ink = inkPixels(iw.currentPage());
    assert(ink >= 4 * 8 * 2);

    // A graphics byte must NOT be masked to 7 bits — bit 7 is pin 8.
    ImageWriter one(144, ImageWriter::PaperSize::Letter);
    feed(one, "\x1b" "E\x1b" "G0001");
    one.printChar(0x80);                    // bottom pin only
    assert(inkPixels(one.currentPage()) > 0);

    // …and the head advanced by exactly one dot at the active density.
    assert(one.status().headX > 0.25);
    assert(one.status().headX - 0.25 - (1.0 / 96.0) < 1e-9);

    // ESC C selects the LQ 3-byte column format (24 pins at 216 dpi).
    ImageWriter lq(144, ImageWriter::PaperSize::Letter);
    feed(lq, "\x1b" "E\x1b" "C0002");
    for (int i = 0; i < 6; ++i) lq.printChar(0xFF);   // 2 columns x 3 bytes
    assert(!lq.status().inGraphics);
    assert(inkPixels(lq.currentPage()) > 0);

    std::printf("  ok: ESC G / ESC C bit images (8- and 24-pin columns)\n");
}

void testCommandFraming()
{
    // ESC R nnn c repeats one character; the odometer counts the 6 bytes
    // that actually crossed the cable, not the 8 characters printed.
    ImageWriter iw(144, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "R008*");
    assert(iw.bytesReceived() == 6);
    const double x = iw.status().headX;
    assert(x - 0.25 - 8.0 / 12.0 < 1e-9 && x > 0.25);   // 8 chars at 12 cpi

    // An unrecognised ESC command swallows only its own command byte —
    // the text after it must still print.
    ImageWriter unk(144, ImageWriter::PaperSize::Letter);
    feed(unk, "\x1b\x01" "A");
    assert(inkPixels(unk.currentPage()) > 0);

    // Pitch commands move both the character width and the graphics density.
    ImageWriter pitch(144, ImageWriter::PaperSize::Letter);
    feed(pitch, "\x1b" "n"); assert(pitch.status().graphicsDpi == 72);
    feed(pitch, "\x1b" "N"); assert(pitch.status().graphicsDpi == 80);
    feed(pitch, "\x1b" "Q"); assert(pitch.status().graphicsDpi == 136);
    assert(pitch.status().cpi == 17.0);

    // Style bits are reflected in the status readout the panel shows.
    feed(pitch, "\x1b" "!");
    assert(pitch.status().styleText.find("bold") != std::string::npos);
    feed(pitch, "\x1b" "\"");
    assert(pitch.status().styleText.find("bold") == std::string::npos);

    std::printf("  ok: ESC R expansion, unknown-command framing, pitch/style\n");
}

void testPaperHandling()
{
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);

    // FORM FEED on a blank sheet must not eject (matches the real button).
    iw.formFeed();
    assert(iw.completedPageCount() == 0);

    feed(iw, "X");
    iw.formFeed();
    assert(iw.completedPageCount() == 1);
    assert(iw.currentPageBlank());
    assert(inkPixels(iw.completedPage(0)) > 0);

    // A guest FF ($0C) ejects too.
    feed(iw, "Y\x0c");
    assert(iw.completedPageCount() == 2);

    // The stack is capped: older sheets roll off and are counted.
    for (size_t i = 0; i < ImageWriter::kMaxPages + 5; ++i) feed(iw, "Z\x0c");
    assert(iw.completedPageCount() == ImageWriter::kMaxPages);
    assert(iw.droppedPageCount() == 7);         // 2 + 37 ejected, 32 kept

    iw.clearAll();
    assert(iw.completedPageCount() == 0);
    assert(iw.droppedPageCount() == 0);
    assert(iw.bytesReceived() == 0);
    assert(iw.currentPageBlank());

    std::printf("  ok: form feed, guest FF, page cap, clear all\n");
}

void testRgbaExport()
{
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    feed(iw, "\x1b" "K0#");

    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(iw.currentPage(), rgba);
    const auto& p = iw.currentPage();
    assert(rgba.size() == static_cast<size_t>(p.w) * p.h * 4);

    bool sawBlackInk = false, sawWhitePaper = false;
    for (size_t i = 0; i < p.pix.size(); ++i) {
        const uint8_t* c = &rgba[i * 4];
        assert(c[3] == 255);                    // opaque everywhere
        if (c[0] == 0 && c[1] == 0 && c[2] == 0)             sawBlackInk   = true;
        if (c[0] == 255 && c[1] == 255 && c[2] == 255)       sawWhitePaper = true;
    }
    assert(sawBlackInk && sawWhitePaper);

    std::printf("  ok: page → RGBA export\n");
}

void testSpoolSeam()
{
    // The UI streams bytes card → printer with drainSpoolFrom(); this is
    // the exact sequence MainWindow::pumpImageWriter() performs.
    PrinterCard card(1);
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    size_t consumed = 0;

    auto pump = [&]() {
        std::vector<uint8_t> fresh;
        if (card.bytesWritten() < consumed) consumed = 0;
        consumed = card.drainSpoolFrom(consumed, fresh);
        if (!fresh.empty()) iw.printBytes(fresh.data(), fresh.size());
        return fresh.size();
    };

    assert(pump() == 0);                        // nothing spooled yet

    card.deviceSelectWrite(1, 'A');
    card.deviceSelectWrite(1, 'B');
    assert(pump() == 2);
    assert(iw.bytesReceived() == 2);
    assert(pump() == 0);                        // no double delivery

    card.deviceSelectWrite(1, 'C');
    assert(pump() == 1);
    assert(iw.bytesReceived() == 3);

    // "Clear spool" in the Printer panel rewinds the card behind our back;
    // the next poll must resynchronise instead of going deaf forever.
    card.clearSpool();
    assert(pump() == 0);
    card.deviceSelectWrite(1, 'D');
    assert(pump() == 1);
    assert(iw.bytesReceived() == 4);

    std::printf("  ok: PrinterCard::drainSpoolFrom streaming + resync\n");
}

} // namespace

int main()
{
    std::printf("ImageWriter smoke test\n");
    testPaperGeometry();
    testTextAndHighBit();
    testColorRibbon();
    testBitImageGraphics();
    testCommandFraming();
    testPaperHandling();
    testRgbaExport();
    testSpoolSeam();
    std::printf("PASS\n");
    return 0;
}
