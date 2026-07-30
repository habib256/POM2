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
#include <cmath>
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

void testMechanismPacing()
{
    // The card delivers a line in one frame; the mechanism prints it at
    // 250 cps draft, so the page must build up over several ticks.
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    iw.setSpeed(ImageWriter::Speed::Draft);

    const char* line = "HELLO WORLD";              // 11 chars ≈ 44 ms
    iw.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    assert(iw.busy());
    assert(iw.pendingBytes() == 11);
    assert(iw.bytesReceived() == 0);               // nothing printed yet

    // One 60 Hz frame buys 16.7 ms ≈ 4 characters — not the whole line.
    iw.tick(1.0 / 60.0);
    const uint64_t afterOneFrame = iw.bytesReceived();
    assert(afterOneFrame > 0 && afterOneFrame < 11);
    assert(iw.busy());

    // Ticking past the line's print time drains it exactly once.
    for (int i = 0; i < 10; ++i) iw.tick(1.0 / 60.0);
    assert(!iw.busy());
    assert(iw.pendingBytes() == 0);
    assert(iw.bytesReceived() == 11);

    // A tick with nothing queued must not bank credit that would later
    // dump a whole line in one frame.
    iw.tick(5.0);
    iw.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    iw.tick(1.0 / 60.0);
    assert(iw.busy());
    iw.flushPending();
    assert(!iw.busy());
    assert(iw.bytesReceived() == 22);

    // NLQ is the slow head: same line, more frames.
    ImageWriter nlq(72, ImageWriter::PaperSize::Letter);
    nlq.setSpeed(ImageWriter::Speed::NLQ);
    nlq.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    nlq.tick(1.0 / 60.0);
    assert(nlq.bytesReceived() < afterOneFrame);

    // Instant is the old behaviour — everything lands the moment it is
    // queued and ticked, and switching to it never strands a job.
    ImageWriter fast(72, ImageWriter::PaperSize::Letter);
    fast.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    fast.setSpeed(ImageWriter::Speed::Instant);
    assert(!fast.busy());
    assert(fast.bytesReceived() == 11);

    // Power-cycling the printer throws the input buffer away with it.
    ImageWriter off(72, ImageWriter::PaperSize::Letter);
    off.queueBytes(reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    off.resetPrinterHard();
    assert(!off.busy() && off.pendingBytes() == 0);
    off.tick(1.0);
    assert(off.bytesReceived() == 0);

    std::printf("  ok: mechanism pacing (draft / NLQ / instant / reset)\n");
}

void testAutoLineFeedDetection()
{
    // The SW A-8 question — feed on CR or not — has three right answers
    // depending on who is sending, and Auto settles it from the stream.
    // Regression: with the printer always feeding, Print Shop's colour
    // passes (separated by a bare CR so they overprint) marched down the
    // page as a coloured staircase instead of landing on one line.
    const double kLine = 1.0 / 6.0;

    // 1. Bare CR (a plain PR#n : PRINT) — the printer must feed, or the
    //    whole listing overprints one line.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        assert(iw.autoFeedMode() == ImageWriter::AutoFeed::Auto);
        assert(iw.autoFeedActive());
        feed(iw, "A\r");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        feed(iw, "B\r");
        assert(std::abs(iw.status().headY - 2 * kLine) < 1e-9);
        assert(!iw.autoFeedLatchedOff());
    }

    // 2. CR+LF (every real driver, and the Grappler+ firmware) — one
    //    advance per line, not two, and the switch latches off.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "A\r\n");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(iw.autoFeedLatchedOff());
        feed(iw, "B\r\n");
        assert(std::abs(iw.status().headY - 2 * kLine) < 1e-9);

        // 3. …and once latched, a bare CR overprints instead of feeding,
        //    which is what a colour pass needs.
        const double y = iw.status().headY;
        feed(iw, "\rC");
        assert(std::abs(iw.status().headY - y) < 1e-9);
        assert(std::abs(iw.status().headX - 0.25) > 1e-9);   // it did print
    }

    // 4. An LF that is NOT preceded by a CR still feeds.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "\n");
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(!iw.autoFeedLatchedOff());
    }

    // 5. Pinning the switch by hand still wins over the detector.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        iw.setAutoFeedMode(ImageWriter::AutoFeed::On);
        feed(iw, "A\r\n");                     // CR feeds, LF swallowed
        assert(std::abs(iw.status().headY - kLine) < 1e-9);
        assert(iw.autoFeedActive());           // stays on: not Auto

        ImageWriter off(72, ImageWriter::PaperSize::Letter);
        off.setAutoFeedMode(ImageWriter::AutoFeed::Off);
        feed(off, "A\r");
        assert(off.status().headY == 0.0);     // never feeds on CR
    }

    // 6. A power cycle re-arms the detector for the next job.
    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        feed(iw, "A\r\n");
        assert(iw.autoFeedLatchedOff());
        iw.resetPrinterHard();
        assert(!iw.autoFeedLatchedOff());
        assert(iw.autoFeedActive());
    }

    std::printf("  ok: line-feed-after-CR detection (bare CR / CR+LF / "
                "overprint)\n");
}

void testNoUnaffordableByte()
{
    // Regression: the credit cap was a flat 1 s, but a form feed near the
    // top of a Letter sheet costs 2.2 s of paper transport — so that byte
    // could never be afforded, the queue stalled forever, and (with BUSY
    // wired back to the card) the guest hung in its firmware ACK loop.
    // Print Shop froze on every page eject.
    ImageWriter iw(72, ImageWriter::PaperSize::Letter);
    iw.setSpeed(ImageWriter::Speed::Draft);
    const uint8_t job[] = { 'H', 'I', 0x0C };       // two chars, then FF
    iw.queueBytes(job, sizeof job);
    for (int f = 0; f < 600; ++f) iw.tick(1.0 / 60.0);   // 10 s of frames
    assert(!iw.busy());
    assert(iw.pendingBytes() == 0);
    assert(iw.completedPageCount() == 1);          // the sheet came out

    // Same for the NLQ carriage return, whose slew from the right margin
    // is also longer than the old cap.
    ImageWriter nlq(72, ImageWriter::PaperSize::Ledger);   // 11 in wide
    nlq.setSpeed(ImageWriter::Speed::NLQ);
    const uint8_t wide[] = { 0x0C, 0x0D };
    nlq.queueBytes(wide, sizeof wide);
    for (int f = 0; f < 900; ++f) nlq.tick(1.0 / 60.0);
    assert(!nlq.busy());

    // And the belt-and-braces watchdog: whatever the cost model says, a
    // byte may not sit unprinted forever. Paper the size of a barn door
    // makes the form feed cost far more than any cap.
    ImageWriter big(72, ImageWriter::PaperSize::A3);
    big.setSpeed(ImageWriter::Speed::NLQ);
    const uint8_t ff[] = { 'X', 0x0C };
    big.queueBytes(ff, sizeof ff);
    for (int f = 0; f < 60 * 60; ++f) big.tick(1.0 / 60.0);   // 60 s
    assert(!big.busy());

    std::printf("  ok: no byte is ever unaffordable (form feed / NLQ CR)\n");
}

void testTraceClosedOnDestruction()
{
    // Regression: the printer owned the trace `FILE*` but had no
    // destructor, and `stopTrace()` was only ever reached from the
    // panel's checkbox. Quitting while tracing therefore stranded the
    // partial hex row inside `traceRow_` — it had never reached stdio, so
    // the C runtime's exit flush could not save it — and the file ended
    // with no footer. On the `POM2_TRACE_PRINTER=1` path (the one used to
    // capture a trace for a bug report) that meant every trace ended
    // truncated, with no way to tell it apart from one cut short by a
    // crash.
    const std::string path = "imagewriter_trace_dtor_test.log";
    std::remove(path.c_str());

    {
        ImageWriter iw(72, ImageWriter::PaperSize::Letter);
        std::string err;
        assert(iw.startTrace(path, err));
        assert(iw.tracing());
        // Fewer than the 16 bytes that force a row flush, so the whole
        // row is still buffered when the printer goes away.
        feed(iw, "HI");
    }   // ← destructor: this is what used to lose the row

    std::FILE* f = std::fopen(path.c_str(), "rb");
    assert(f);
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    std::fclose(f);
    std::remove(path.c_str());

    // The buffered row reached the file...
    assert(body.find("48 49") != std::string::npos);   // 'H' 'I'
    assert(body.find("|HI|") != std::string::npos);
    // ...and the trace says it ended on purpose.
    assert(body.find("# trace closed after 2 bytes") != std::string::npos);

    std::printf("  ok: trace flushed + closed when the printer is destroyed\n");
}

// 12. Parser hardening (bug-hunt 2026-07-28). Three ways a hostile or
//     corrupted stream used to wedge or corrupt the parser:
//       a) a non-digit inside an ESC G count went negative and the
//          uint32_t cast turned it into ~4 G bytes of graphics data —
//          the printer went deaf for the rest of the session;
//       b) ESC ' / ESC I left the previous command's parameter count
//          armed, so the next 1-6 printable characters were swallowed;
//       c) ESC r (reverse feed) + LFs walked the head to negative Y.
void testParserHardening()
{
    // a) ESC G with a corrupted digit must not wedge in graphics mode.
    {
        ImageWriter iw;
        feed(iw, "\x1BG0-10");                 // '-' is not a digit
        // Worst case the clamped count (0x0y10-ish) eats a few bytes —
        // definitely not 4 billion. Feed a small payload then text.
        for (int i = 0; i < 512; ++i) { const uint8_t b = 0xFF; iw.printBytes(&b, 1); }
        assert(!iw.status().inGraphics);
        feed(iw, "TEXT\r");
        assert(inkPixels(iw.currentPage()) > 0);
    }

    // b) ESC I right after a parametered command must not eat text.
    //    Identical streams except for the (unsupported, zero-parameter)
    //    ESC I must produce identical ink — the stale parameter count
    //    used to swallow "HELL".
    {
        ImageWriter iw, ref;
        feed(iw,  "\x1BG0004\xFF\xFF\xFF\xFF\x1BIHELLO\r");
        feed(ref, "\x1BG0004\xFF\xFF\xFF\xFF"     "HELLO\r");
        assert(inkPixels(iw.currentPage()) == inkPixels(ref.currentPage()));
    }

    // c) Reverse feed clamps at the top edge and keeps paying (positive)
    //    paper-transport cost.
    {
        ImageWriter iw;
        feed(iw, "X\r\n\x1Br");                // one line down, reverse
        for (int i = 0; i < 50; ++i) feed(iw, "\n");
        assert(iw.status().headY >= 0.0);
    }

    std::printf("  ok: corrupted counts, ESC I framing and reverse feed "
                "are all bounded\n");
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
    testMechanismPacing();
    testAutoLineFeedDetection();
    testNoUnaffordableByte();
    testTraceClosedOnDestruction();
    testParserHardening();
    std::printf("PASS\n");
    return 0;
}
