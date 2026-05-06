// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Software framebuffer for the Apple II video subsystem. Renders into a
// 280×192 RGBA buffer that the UI uploads as an OpenGL texture each frame.
// Three modes follow the soft-switch state held by Memory:
//   - Text  (40×24, char ROM glyphs, normal / inverse / flashing)
//   - Lo-res (40×48, 16 colours, same screen memory as text)
//   - Hi-res (280×192, NTSC artifact decoded by a 7-bit sliding window over
//             a linearised 560-sub-pixel bit stream — MSB-driven half-dot
//             delay applied at the stream level so byte-boundary fringing
//             emerges naturally)
// Mixed mode shows hi-res in the top 160 pixels and four text rows below.
//
// The display owns no GL state — the MainWindow uploads `pixels()` to a
// texture it manages. Keeping this class GL-free makes it trivial to unit
// test and lets a future WASM build reuse the exact same renderer.

#ifndef POM2_APPLE2_DISPLAY_H
#define POM2_APPLE2_DISPLAY_H

#include <array>
#include <cstdint>
#include <vector>

class Memory;
class LeChatMauveCard;

class Apple2Display
{
public:
    static constexpr int kWidth  = 280;
    static constexpr int kHeight = 192;

    // Hi-res rendering style. ColorNTSC is the "real Apple II on a colour TV"
    // experience — bit-stream artifact colour with authentic byte-boundary
    // fringing. ChatMauveRGB is the French Péritel-RGB experience (clean
    // 16-color decode, two distinct grays, no fringing) — only available
    // when a LeChatMauveCard is plugged on the slot bus, and its FIFO
    // mode (BW560 / Mixed / Chunky / COL140) selects the sub-variant.
    // The three Mono variants render the same bit stream as luminance
    // through a phosphor tint: White is a reference monitor, Green
    // approximates Apple's standard P31 CRT, Amber adds long persistence
    // (history-buffer lerp) on top of an amber tint.
    enum class HiResMode {
        ColorNTSC,
        ChatMauveRGB,
        MonoWhite,
        MonoGreen,
        MonoAmber,
    };

    Apple2Display();

    // Renders the current frame into the internal RGBA buffer based on
    // the soft-switch state read from `mem`. Frame-counter advances the
    // 2 Hz flash phase used by the text mode.
    void render(Memory& mem);

    const uint32_t* pixels() const { return frame.data(); }
    int             width()  const { return kWidth;  }
    int             height() const { return kHeight; }

    // Cursor row/col for the on-screen blinking caret. Apple II monitors
    // park the cursor at $0024-$0025 zero-page; we just read those bytes.
    void setCursorOverlay(bool on) { cursorOverlay = on; }

    /// Hi-res rendering mode. Switching modes resets the persistence buffer
    /// so an amber afterglow doesn't bleed into a freshly-selected green
    /// phosphor.
    void      setHiResMode(HiResMode m);
    HiResMode getHiResMode() const { return hiResMode; }

    /// Le Chat Mauve / Video-7 RGB card. When non-null AND its render mode
    /// is anything other than NTSC-passthrough, the hi-res renderer takes
    /// the clean-RGB path: 4-bit-window palette decode (no artifact LUT,
    /// no inter-byte fringing, two distinct grays for the $5/$A patterns).
    /// The pointer is non-owning; the card lifetime is managed by the
    /// SlotBus that holds it.
    void setChatMauveCard(LeChatMauveCard* c) { chatMauve = c; }

private:
    std::vector<uint32_t> frame;   // kWidth * kHeight RGBA pixels
    bool cursorOverlay  = true;
    HiResMode hiResMode = HiResMode::ColorNTSC;
    LeChatMauveCard* chatMauve = nullptr;   // non-owning, owned by SlotBus
    // History buffer for monochrome phosphor decay. One byte per pixel
    // (0..255 luminance). Color mode leaves this untouched; switching modes
    // clears it.
    std::vector<uint8_t> persistenceL;
    // Frame counter — drives the FLASH attribute animation for screen
    // bytes in the $40-$7F range (the Apple II Monitor's blinking cursor
    // and inverse-blinking spaces). Wraps freely; only the parity of
    // (frameCounter / kFlashHalfPeriodFrames) is read.
    uint32_t frameCounter = 0;
    static constexpr uint32_t kFlashHalfPeriodFrames = 30;  // 0.5 s at 60 Hz

    void renderText (Memory& mem, int firstRow, int lastRow);
    void renderLoRes(Memory& mem, int firstRow, int lastRow);
    void renderHiRes(Memory& mem, int firstScanline, int lastScanline);

    // Address of the first byte of text/lo-res row `y` in the active page.
    static uint16_t textRowAddress(int y, bool page2);
    // Address of the first byte of HGR scanline `y` in the active page.
    static uint16_t hgrRowAddress(int y, bool page2);

    // Apple II lo-res palette (16 colours, IIGS-corrected approximation).
    static const uint32_t kLoResPalette[16];
    // Le Chat Mauve / Video-7 lo-res palette — 16 distinct colours with
    // visually distinct grays at indices 5 and 10 (the "Chat Mauve
    // trademark" actually surfaces in lo-res, not HGR — see Apple2Display.cpp).
    static const uint32_t kChatMauveLoResPalette[16];
};

#endif // POM2_APPLE2_DISPLAY_H
