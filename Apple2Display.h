// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Software framebuffer for the Apple II video subsystem. Renders into a
// 280×192 RGBA buffer that the UI uploads as an OpenGL texture each frame.
// Three modes follow the soft-switch state held by Memory:
//   - Text  (40×24, char ROM glyphs, normal / inverse / flashing)
//   - Lo-res (40×48, 16 colours, same screen memory as text)
//   - Hi-res (280×192, 6-colour NTSC palette per byte / pixel parity)
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

class Apple2Display
{
public:
    static constexpr int kWidth  = 280;
    static constexpr int kHeight = 192;

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

    /// Hi-res glow — soft horizontal halo around lit HGR pixels. Costs an
    /// extra 280-wide pass per scanline; disabling it gives a sharper but
    /// more "digital" look. Default ON to match the CRT feel of the era.
    void setHiResGlow(bool on) { hgrGlowEnabled = on; }
    bool getHiResGlow() const  { return hgrGlowEnabled; }

private:
    std::vector<uint32_t> frame;   // kWidth * kHeight RGBA pixels
    bool cursorOverlay  = true;
    bool hgrGlowEnabled = true;
    // Frame counter — drives the FLASH attribute animation for screen
    // bytes in the $40-$7F range (the Apple II Monitor's blinking cursor
    // and inverse-blinking spaces). Wraps freely; only the parity of
    // (frameCounter / kFlashHalfPeriodFrames) is read.
    uint32_t frameCounter = 0;
    static constexpr uint32_t kFlashHalfPeriodFrames = 30;  // 0.5 s at 60 Hz

    void renderText (Memory& mem, int firstRow, int lastRow);
    void renderLoRes(Memory& mem, int firstRow, int lastRow);
    void renderHiRes(Memory& mem, int firstScanline, int lastScanline);

    // Apply the horizontal additive glow filter to a 280-wide row of raw
    // HGR pixels and write the result to `dst`. Lit pixels pass through
    // unchanged; black pixels surrounded by lit neighbours pick up a halo.
    static void applyHgrGlow(const uint32_t* src, uint32_t* dst);

    // Address of the first byte of text/lo-res row `y` in the active page.
    static uint16_t textRowAddress(int y, bool page2);
    // Address of the first byte of HGR scanline `y` in the active page.
    static uint16_t hgrRowAddress(int y, bool page2);

    // Apple II lo-res palette (16 colours, IIGS-corrected approximation).
    static const uint32_t kLoResPalette[16];
};

#endif // POM2_APPLE2_DISPLAY_H
