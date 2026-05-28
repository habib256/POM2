// POM2 Apple II Emulator
// Copyright (C) 2026
//
// OpenEmulator-inspired NTSC composite simulation as a GLSL shader pass.
// Consumes a 560×192 R8 luminance signal produced by Apple2Display when
// hiResMode == ColorCompositeOE, demodulates Y/I/Q from the 14.318 MHz
// subcarrier, low-pass filters chroma, applies user knobs (brightness,
// contrast, saturation, hue, sharpness, persistence, scanlines, barrel),
// and renders into an RGBA framebuffer texture MainWindow then draws via
// ImGui::Image() in place of the regular `screenTexture`.
//
// The algorithm is reimplemented from public NTSC spec (FCC/CCIR §73.682
// composite encoding), Linards Ticmanis' Apple II video timing notes,
// and Zellyn Hunter's openemulator-explainer notebook. No OpenEmulator /
// libemulation source is copied — POM2 stays MIT-licensed.

#ifndef POM2_NTSC_POSTPROCESSOR_H
#define POM2_NTSC_POSTPROCESSOR_H

#include <cstdint>
#include <string>

namespace pom2 {

struct NtscParams
{
    // Standard composite-TV knobs (range 0..1 unless noted).
    float brightness  = 0.0f;   // -0.5..+0.5 added to luma
    float contrast    = 1.0f;   //  0.5..1.5 scaling around 0.5
    float saturation  = 1.0f;   //  0..2 chroma multiplier
    float hue         = 0.0f;   // -0.5..+0.5, full I/Q rotation at ±0.5

    // Sharpness controls the chroma low-pass bandwidth: 0 = blurry
    // bandwidth-limited TV, 1 = sharp composite monitor (RGB-ish).
    float sharpness   = 0.5f;

    // Phosphor persistence: 0 = no afterglow, 1 = infinite. Reasonable
    // CRT values are 0.3..0.6.
    float persistence = 0.4f;

    // Scanlines + barrel are pure post-effects (no NTSC physics).
    float scanlines   = 0.25f;  // 0 = off, 1 = black between every line
    float barrel      = 0.05f;  // 0 = flat, 0.2 = old curved CRT

    // Shadow-mask emulation (post-effect, after demodulation). The mask
    // is a multiplicative pattern in RGB space — triad (3-stripe RGB
    // mask of consumer TVs), aperture grille (vertical RGB stripes of
    // Trinitron/Sony), or Bayer-like dot-mask (offset triads). Strength
    // 0 = off (the GPU bypasses the mask multiplication entirely);
    // strength 1 = full darkening of the off-channels in each cell.
    enum class ShadowMask : int {
        Off            = 0,
        Triad          = 1,   // classic 3-stripe shadow mask
        ApertureGrille = 2,   // Trinitron vertical stripes
        Dot            = 3,   // offset triads (consumer CRT)
    };
    ShadowMask shadowMask         = ShadowMask::Off;
    float      shadowMaskStrength = 0.5f;  // 0..1

    // PAL composite mode: alternates the Q-subcarrier sign every other
    // scanline (line-phase alternation). On a real PAL TV this cancels
    // hue errors at the cost of vertical chroma resolution; here it
    // produces the characteristic softer-coloured European Apple II
    // look. Off by default — POM2 ships defaults that match the NTSC
    // Apple II that 90% of users have in mind.
    bool palMode = false;

    // When ColorCompositeOE is selected and the Apple II is in TEXT
    // mode (40 or 80 col), this toggle decides whether the shader
    // demodulates the glyph bit-stream (composite-faithful, but blurry
    // for white-on-black text) or whether MainWindow draws the sharp
    // RGB framebuffer directly (legible, but breaks immersion). On by
    // default — readability wins for daily use.
    bool textSharp = true;
};

class NtscPostProcessor
{
public:
    NtscPostProcessor();
    ~NtscPostProcessor();
    NtscPostProcessor(const NtscPostProcessor&) = delete;
    NtscPostProcessor& operator=(const NtscPostProcessor&) = delete;

    // First-call setup. Compiles the shader, allocates the signal
    // texture, FBOs and a fullscreen-quad VAO. Returns true on success;
    // on failure (shader compile error, GL entry points missing, …) the
    // postprocessor reports `available()` = false and process() becomes
    // a no-op so callers can fall through to the regular RGB path.
    // Must be called with a current GL context. Safe to call multiple
    // times — second and later calls are no-ops.
    bool initialize();

    bool available() const { return ready; }

    // Replace the live parameter set. Cheap (only writes a struct
    // copy); the new values take effect on the next process() call.
    void setParams(const NtscParams& p) { params = p; }
    const NtscParams& getParams() const { return params; }

    // Run one frame of the NTSC simulation. `signal` points to a
    // signalWidth × signalHeight R8 buffer (typically 560×192 = the
    // Apple II's 4×-subcarrier sample grid). Returns the GL texture
    // name holding the RGBA output (the caller draws it via ImGui).
    // Returns 0 if the postprocessor isn't `available()`.
    unsigned int process(const uint8_t* signal,
                         int signalWidth, int signalHeight);

    // Dimensions of the texture returned by process(). The output is
    // upscaled vertically by 2 so scanline darkening at every other
    // output row stays crisp; horizontally we keep the native 4×-fsc
    // sample rate which is already plenty.
    int outputWidth () const { return outW; }
    int outputHeight() const { return outH; }

    // Diagnostics for the Settings panel.
    const std::string& lastError() const { return errorMsg; }

private:
    bool   ready         = false;
    bool   initialized   = false;
    std::string errorMsg;

    // GL objects (declared unsigned int to keep this header GL-include
    // free; the cpp casts as needed).
    unsigned int program      = 0;
    unsigned int signalTex    = 0;
    unsigned int outputTex[2] = {0, 0};   // ping-pong for persistence
    unsigned int fbo[2]       = {0, 0};
    unsigned int vao          = 0;
    unsigned int vbo          = 0;

    // Uniform locations resolved at link time. -1 if absent (driver
    // optimised the uniform out — value writes become no-ops).
    int uSignal      = -1;
    int uPrevFrame   = -1;
    int uBrightness  = -1;
    int uContrast    = -1;
    int uSaturation  = -1;
    int uHue         = -1;
    int uSharpness   = -1;
    int uPersistence = -1;
    int uScanlines   = -1;
    int uBarrel      = -1;
    int uSignalSize  = -1;
    int uShadowMask  = -1;
    int uShadowStr   = -1;
    int uPalMode     = -1;

    int outW    = 0;
    int outH    = 0;
    int signalW = 0;
    int signalH = 0;
    int pingPongIdx = 0;
    bool firstFrame = true;

    NtscParams params{};

    bool createTextures(int signalW_, int signalH_);
    void destroyGL();
};

} // namespace pom2

#endif // POM2_NTSC_POSTPROCESSOR_H
