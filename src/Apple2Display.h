// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Software framebuffer for the Apple II video subsystem. Renders into an
// RGBA buffer that the UI uploads as an OpenGL texture each frame. Three
// modes follow the soft-switch state held by Memory:
//   - Text  (40×24, char ROM glyphs, normal / inverse / flashing)
//   - Lo-res (40×48, 16 colours, same screen memory as text)
//   - Hi-res (280×192, NTSC artifact decoded by a 7-bit sliding window over
//             a linearised 560-sub-pixel bit stream — MSB-driven half-dot
//             delay applied at the stream level so byte-boundary fringing
//             emerges naturally)
// Mixed mode shows hi-res in the top 160 pixels and four text rows below.
//
// IIe extension. When 80COL + TEXT are on the renderer emits at the native
// 80-col resolution (560×192) by interleaving aux RAM (even columns) and
// main RAM (odd columns) text bytes. Mixed-mode bottom-4-rows-text follows
// the same path, with the top 20 HGR rows horizontally doubled into the
// 560-wide buffer. width()/height() reflect whichever buffer is live.
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
    static constexpr int kWidth   = 280;
    static constexpr int kHeight  = 192;
    static constexpr int kWidth80 = 560;

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
        ColorNTSC,              // MAME composite_color_mode=0, LUT row 0
        ColorCompMedium,        // MAME composite_color_mode=1, LUT row 1
                                //   (4n medium-color runs; uglier 40-col
                                //    text but cleaner mid-tones)
        ColorComp4Bit,          // MAME composite_color_mode=2, no artifact —
                                //   each 4-dot nibble maps directly to one
                                //   palette index. The sharp / hard-edge
                                //   variant.
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

    const uint32_t* pixels() const { return useFrame80 ? frame80.data() : frame.data(); }
    int             width()  const { return useFrame80 ? kWidth80 : kWidth; }
    int             height() const { return kHeight; }

    /// Auxiliary 64 KB RAM pointer for IIe 80-column rendering. Set by
    /// MainWindow once the IIe ROM is detected and Memory::setIIEMode(true)
    /// has been called. Pointer is non-owning. May be nullptr; the 80-col
    /// path then falls back to reading main RAM in both halves of each pair
    /// (still produces 80 character cells, just without aux content).
    void setAuxMemory(const uint8_t* aux) { auxRam = aux; }

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
    std::vector<uint32_t> frame;     // kWidth   * kHeight RGBA pixels
    std::vector<uint32_t> frame80;   // kWidth80 * kHeight RGBA pixels (IIe)
    bool useFrame80     = false;     // true for the current frame when 80-col
    const uint8_t* auxRam = nullptr; // IIe auxiliary RAM (non-owning)
    HiResMode hiResMode = HiResMode::ColorNTSC;
    LeChatMauveCard* chatMauve = nullptr;   // non-owning, owned by SlotBus
    // History buffer for monochrome phosphor decay. One byte per pixel
    // (0..255 luminance). Color mode leaves this untouched; switching modes
    // clears it. We carry two parallel buffers so HGR (280×192) and DHGR
    // (560×192) can each accumulate afterglow without their dimensions
    // fighting — without `persistenceL80`, DHGR mono had no decay because
    // the path wrote a 560-wide frame against a 280-wide history.
    std::vector<uint8_t> persistenceL;
    std::vector<uint8_t> persistenceL80;
    // Frame counter — drives the FLASH attribute animation for screen
    // bytes in the $40-$7F range (the Apple II Monitor's blinking cursor
    // and inverse-blinking spaces). Wraps freely; only the parity of
    // (frameCounter / kFlashHalfPeriodFrames) is read.
    uint32_t frameCounter = 0;
    // Half-period of the inverse-flashing animation. 15 frames @ 60 Hz =
    // 250 ms → 2 Hz cycle, matching MAME's `screen.frame_number() & 0x10`
    // model (Apple II/II+ 555-timer).
    static constexpr uint32_t kFlashHalfPeriodFrames = 15;

    void renderText  (Memory& mem, int firstRow, int lastRow);
    void renderLoRes (Memory& mem, int firstRow, int lastRow);
    void renderHiRes (Memory& mem, int firstScanline, int lastScanline);
    // HGR + Le Chat Mauve (RGB-card) at the card's native 560-dot
    // resolution. Same decode algorithm as the Chat Mauve branch of
    // `renderHiRes`, but writes into `frame80` so screen captures and
    // future MSB half-dot-delay work see the full pixel density.
    // Each 4-dot color pair → 4 identical output dots; each BW560 input
    // dot → 2 identical output dots. (The on-screen output of GL
    // upscaling is identical to the 280-wide path in the current model;
    // the gain is in the framebuffer fidelity itself.)
    void renderHiResChatMauve80(Memory& mem, int firstScanline, int lastScanline);
    // Le Chat Mauve Eve "HGR Duochrome". Single-HGR resolution image
    // bitmap lives in MAIN $2000-$3FFF (one bit per pixel as usual);
    // at the matching offset in AUX, each byte holds a per-7-pixel-block
    // colour pair (high nibble = foreground lo-res palette index, low
    // nibble = background). Each HGR pixel becomes 2 dots in frame80.
    // Renders into `frame80` (560 wide); the gate at the top of render()
    // requires `auxRam != nullptr` so this path can read the aux bytes.
    void renderHgrDuochrome(Memory& mem, int firstScanline, int lastScanline);
    // IIe-only. Renders text rows [firstRow, lastRow) into `frame80` at
    // 560×192. Reads aux RAM for even columns and main RAM for odd
    // columns (per AppleWin's scanner). `altCharSet` toggles flashing
    // inverse vs. mousetext+non-flashing inverse (the IIe ALTCHAR switch).
    void renderText80(Memory& mem, int firstRow, int lastRow, bool altCharSet);
    // IIe-only. Renders DHGR scanlines [firstScanline, lastScanline) into
    // `frame80`. Reads main + aux HGR pages: aux byte at offset c
    // contributes 7 bits to dots [c*14 .. c*14+6], main byte contributes
    // dots [c*14+7 .. c*14+13]. Color: each 4 consecutive dots form a
    // 4-bit lo-res palette index (560 dots → 140 color cells per line).
    // Monochrome HiResModes render dot-by-dot luminance through the
    // selected phosphor.
    void renderDhgr  (Memory& mem, int firstScanline, int lastScanline);
    // Le Chat Mauve / Video-7 "foreground-background" colored TEXT mode.
    // Active on a IIe-class machine with the RGB card in 40-col text while
    // the DHGR (AN3) soft-switch is on. Char code comes from main RAM; the
    // aux byte at the same text address holds the cell colours (high nibble
    // = foreground, low nibble = background, both lo-res palette indices).
    // The 7-bit glyph row is doubled to 14 dots, each dot painted fg/bg.
    // Renders rows [firstRow, lastRow) into `frame80` at 560 wide. Port of
    // MAME `apple2video.cpp` text_update (:788-791) + render_line_color_array
    // (:571-583).
    void renderTextChatMauveFgBg(Memory& mem, int firstRow, int lastRow);
    // Horizontally double `frame[firstRow*8 .. lastRow*8)` into `frame80`.
    // Used when mixed-mode HGR is on top and 80-col text is at the bottom.
    void upscaleFrameToFrame80(int firstScanline, int lastScanline);

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
