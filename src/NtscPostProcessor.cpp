// POM2 Apple II Emulator
// Copyright (C) 2026

#include "NtscPostProcessor.h"
#include "OpenGLShader.h"
#include "Logger.h"

#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
#  include <GLES3/gl3.h>
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
#  include <GL/gl.h>
#  include <GL/glext.h>
#  include <GLFW/glfw3.h>

// Lazily-loaded GL 2.0+ entry points (Linux/Windows path). We use the
// same dynamic-loader strategy as OpenGLShader.cpp — see the comment
// there for the rationale.
namespace {
PFNGLGENFRAMEBUFFERSPROC        glGenFramebuffers_        = nullptr;
PFNGLBINDFRAMEBUFFERPROC        glBindFramebuffer_        = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC   glFramebufferTexture2D_   = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_ = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC     glDeleteFramebuffers_     = nullptr;
PFNGLGENVERTEXARRAYSPROC        glGenVertexArrays_        = nullptr;
PFNGLBINDVERTEXARRAYPROC        glBindVertexArray_        = nullptr;
PFNGLDELETEVERTEXARRAYSPROC     glDeleteVertexArrays_     = nullptr;
PFNGLGENBUFFERSPROC             glGenBuffers_             = nullptr;
PFNGLBINDBUFFERPROC             glBindBuffer_             = nullptr;
PFNGLBUFFERDATAPROC             glBufferData_             = nullptr;
PFNGLDELETEBUFFERSPROC          glDeleteBuffers_          = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_ = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC    glVertexAttribPointer_    = nullptr;
PFNGLUSEPROGRAMPROC             glUseProgram_             = nullptr;
PFNGLGETUNIFORMLOCATIONPROC     glGetUniformLocation_     = nullptr;
PFNGLUNIFORM1IPROC              glUniform1i_              = nullptr;
PFNGLUNIFORM1FPROC              glUniform1f_              = nullptr;
PFNGLUNIFORM2FPROC              glUniform2f_              = nullptr;
PFNGLACTIVETEXTUREPROC          glActiveTexture_          = nullptr;
bool entryPointsLoaded_ = false;
bool loadEntryPoints()
{
    if (entryPointsLoaded_) return true;
    auto get = [](const char* n) {
        return reinterpret_cast<void*>(glfwGetProcAddress(n));
    };
#define LOAD(t, v, n) v = reinterpret_cast<t>(get(n))
    LOAD(PFNGLGENFRAMEBUFFERSPROC,        glGenFramebuffers_,        "glGenFramebuffers");
    LOAD(PFNGLBINDFRAMEBUFFERPROC,        glBindFramebuffer_,        "glBindFramebuffer");
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC,   glFramebufferTexture2D_,   "glFramebufferTexture2D");
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus_, "glCheckFramebufferStatus");
    LOAD(PFNGLDELETEFRAMEBUFFERSPROC,     glDeleteFramebuffers_,     "glDeleteFramebuffers");
    LOAD(PFNGLGENVERTEXARRAYSPROC,        glGenVertexArrays_,        "glGenVertexArrays");
    LOAD(PFNGLBINDVERTEXARRAYPROC,        glBindVertexArray_,        "glBindVertexArray");
    LOAD(PFNGLDELETEVERTEXARRAYSPROC,     glDeleteVertexArrays_,     "glDeleteVertexArrays");
    LOAD(PFNGLGENBUFFERSPROC,             glGenBuffers_,             "glGenBuffers");
    LOAD(PFNGLBINDBUFFERPROC,             glBindBuffer_,             "glBindBuffer");
    LOAD(PFNGLBUFFERDATAPROC,             glBufferData_,             "glBufferData");
    LOAD(PFNGLDELETEBUFFERSPROC,          glDeleteBuffers_,          "glDeleteBuffers");
    LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray_, "glEnableVertexAttribArray");
    LOAD(PFNGLVERTEXATTRIBPOINTERPROC,    glVertexAttribPointer_,    "glVertexAttribPointer");
    LOAD(PFNGLUSEPROGRAMPROC,             glUseProgram_,             "glUseProgram");
    LOAD(PFNGLGETUNIFORMLOCATIONPROC,     glGetUniformLocation_,     "glGetUniformLocation");
    LOAD(PFNGLUNIFORM1IPROC,              glUniform1i_,              "glUniform1i");
    LOAD(PFNGLUNIFORM1FPROC,              glUniform1f_,              "glUniform1f");
    LOAD(PFNGLUNIFORM2FPROC,              glUniform2f_,              "glUniform2f");
    LOAD(PFNGLACTIVETEXTUREPROC,          glActiveTexture_,          "glActiveTexture");
#undef LOAD
    entryPointsLoaded_ =
        glGenFramebuffers_ && glBindFramebuffer_ && glFramebufferTexture2D_ &&
        glCheckFramebufferStatus_ && glDeleteFramebuffers_ &&
        glGenVertexArrays_ && glBindVertexArray_ && glDeleteVertexArrays_ &&
        glGenBuffers_ && glBindBuffer_ && glBufferData_ && glDeleteBuffers_ &&
        glEnableVertexAttribArray_ && glVertexAttribPointer_ &&
        glUseProgram_ && glGetUniformLocation_ &&
        glUniform1i_ && glUniform1f_ && glUniform2f_ && glActiveTexture_;
    return entryPointsLoaded_;
}
} // namespace
#  define glGenFramebuffers        glGenFramebuffers_
#  define glBindFramebuffer        glBindFramebuffer_
#  define glFramebufferTexture2D   glFramebufferTexture2D_
#  define glCheckFramebufferStatus glCheckFramebufferStatus_
#  define glDeleteFramebuffers     glDeleteFramebuffers_
#  define glGenVertexArrays        glGenVertexArrays_
#  define glBindVertexArray        glBindVertexArray_
#  define glDeleteVertexArrays     glDeleteVertexArrays_
#  define glGenBuffers             glGenBuffers_
#  define glBindBuffer             glBindBuffer_
#  define glBufferData             glBufferData_
#  define glDeleteBuffers          glDeleteBuffers_
#  define glEnableVertexAttribArray glEnableVertexAttribArray_
#  define glVertexAttribPointer    glVertexAttribPointer_
#  define glUseProgram             glUseProgram_
#  define glGetUniformLocation     glGetUniformLocation_
#  define glUniform1i              glUniform1i_
#  define glUniform1f              glUniform1f_
#  define glUniform2f              glUniform2f_
#  define glActiveTexture          glActiveTexture_
#endif

namespace pom2 {

#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
namespace { bool loadEntryPoints() { return true; } }
#endif

namespace {

// Fullscreen-quad vertex shader: passes UV in [0..1] to the fragment.
const char* kVertexShader = R"GLSL(
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Fragment shader: NTSC composite decode + post-effects.
//
// Pipeline per output fragment:
//   1. Optional barrel distortion of the UV (curved-CRT look).
//   2. For Y/I/Q demodulation we sample N+1 taps around the current
//      column in the signal texture. Each tap contributes Y directly
//      (gaussian-weighted, narrow sigma → sharp luma); for chroma we
//      multiply by sin/cos of the 4×fsc subcarrier phase at the tap's
//      x position (phase = π/2 per dot — the Apple II's pixel clock IS
//      the colorburst clock) and accumulate with a WIDER gaussian
//      (sharpness slider controls the chroma bandwidth).
//   3. Standard NTSC YIQ→RGB matrix, then hue rotation in IQ plane.
//   4. Brightness (additive), contrast (scale around 0.5), saturation
//      (lerp toward luma).
//   5. Persistence blend with the previous frame's output (max-style
//      so bright pixels glow instead of greying).
//   6. Scanlines via a multiplicative darkening of odd output rows.
const char* kFragmentShader = R"GLSL(
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uSignal;
uniform sampler2D uPrev;
uniform vec2  uSignalSize;     // (width, height) of the signal texture
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uSharpness;
uniform float uPersistence;
uniform float uScanlines;
uniform float uBarrel;
uniform int   uShadowMask;     // 0=off, 1=triad, 2=aperture grille, 3=dot
uniform float uShadowStrength; // 0..1
uniform int   uPalMode;        // 0=NTSC, 1=PAL (line-phase alternation)

const float PI = 3.14159265358979;

float sampleSignal(float x, float y)
{
    if (x < 0.0 || x >= uSignalSize.x || y < 0.0 || y >= uSignalSize.y) return 0.0;
    return texture(uSignal, vec2(x / uSignalSize.x, y / uSignalSize.y)).r;
}

void main()
{
    // ── Barrel distortion ─────────────────────────────────────────
    vec2 cuv = vUv * 2.0 - 1.0;
    float r2 = dot(cuv, cuv);
    vec2 buv = cuv * (1.0 + uBarrel * r2);
    vec2 uv  = buv * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float sigX = uv.x * uSignalSize.x;
    float sigY = uv.y * uSignalSize.y;

    // ── PAL line-phase alternation ────────────────────────────────
    // PAL inverts the Q chroma component every other scanline. We
    // simulate that by flipping the Q-tap sign when the source line is
    // odd. Implemented as a Q-multiplier (±1) that NTSC keeps at +1.
    float palQSign = 1.0;
    if (uPalMode == 1) {
        float lineIdx = floor(sigY);
        palQSign = (mod(lineIdx, 2.0) < 1.0) ? 1.0 : -1.0;
    }

    // ── Y / I / Q accumulation over a small kernel ────────────────
    // sigmaY narrow → sharp luminance.
    // sigmaC controlled by sharpness: high sharpness → narrow chroma
    // filter → ringy / sharp colour; low sharpness → wide filter →
    // soft pastel (the OE "TV" look).
    float sigmaY = 0.8;
    float sigmaC = mix(2.5, 1.0, clamp(uSharpness, 0.0, 1.0));

    const int N = 8;
    float Y = 0.0, I = 0.0, Q = 0.0;
    float wYs = 0.0, wCs = 0.0;
    for (int i = -N; i <= N; ++i) {
        float fx = sigX + float(i);
        float s  = sampleSignal(fx, sigY);
        float dy = float(i);
        float wY = exp(-0.5 * dy * dy / (sigmaY * sigmaY));
        float wC = exp(-0.5 * dy * dy / (sigmaC * sigmaC));

        // 4×-fsc phase: one full subcarrier cycle every 4 dots.
        float phase = PI * 0.5 * fx;

        Y   += s * wY;
        I   += s * sin(phase) * wC * 2.0;
        Q   += s * cos(phase) * wC * 2.0 * palQSign;
        wYs += wY;
        wCs += wC;
    }
    Y /= wYs;
    I /= wCs;
    Q /= wCs;

    // ── Hue rotation (IQ plane) ───────────────────────────────────
    float h = uHue * PI;
    float cs = cos(h), sn = sin(h);
    float Ir = I * cs - Q * sn;
    float Qr = I * sn + Q * cs;

    // ── YIQ → RGB (NTSC matrix) ───────────────────────────────────
    vec3 rgb = vec3(
        Y + 0.956 * Ir + 0.621 * Qr,
        Y - 0.272 * Ir - 0.647 * Qr,
        Y - 1.106 * Ir + 1.703 * Qr
    );

    // ── Brightness / contrast / saturation ────────────────────────
    rgb = (rgb - 0.5) * uContrast + 0.5 + uBrightness;
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(luma), rgb, clamp(uSaturation, 0.0, 4.0));
    rgb = clamp(rgb, 0.0, 1.0);

    // ── Persistence (CRT phosphor decay) ──────────────────────────
    vec3 prev = texture(uPrev, vUv).rgb;
    rgb = max(rgb, prev * clamp(uPersistence, 0.0, 0.98));

    // ── Scanlines (post) ──────────────────────────────────────────
    // Output texture is 2× vertical of signal, so signal-row parity ≠
    // output-row parity. Darken odd output rows (works at 2× vertical).
    float outRow = uv.y * (uSignalSize.y * 2.0);
    float scan = 1.0 - uScanlines * fract(outRow * 0.5) * 2.0;
    if (mod(floor(outRow), 2.0) < 1.0) scan = 1.0;
    rgb *= scan;

    // ── Shadow mask (post) ────────────────────────────────────────
    // Procedural mask in output-pixel space. Approximates the
    // multiplicative attenuation of off-channels in each CRT cell —
    // not the additive luminance/halation that a real mask also
    // imparts (a future per-channel glow pass could handle that).
    //   Triad         — 3-pixel-wide RGB stripes, no vertical offset.
    //   ApertureGrille — same stripes, no inter-line gap (Trinitron).
    //   Dot           — same RGB rotation but each row shifts by 1
    //                    triplet width, giving a quincunx pattern.
    if (uShadowMask != 0 && uShadowStrength > 0.0) {
        float ox = uv.x * (uSignalSize.x * 2.0); // output X in pixels
        if (uShadowMask == 3) {
            ox += (mod(floor(outRow * 0.5), 2.0) < 1.0) ? 0.0 : 1.5;
        }
        int phase = int(mod(floor(ox), 3.0));
        vec3 maskColor = vec3(0.0);
        if      (phase == 0) maskColor = vec3(1.0, 0.0, 0.0);
        else if (phase == 1) maskColor = vec3(0.0, 1.0, 0.0);
        else                 maskColor = vec3(0.0, 0.0, 1.0);
        vec3 atten = mix(vec3(1.0), maskColor, uShadowStrength);
        // Triad has horizontal gaps between cell rows; aperture grille
        // does not. We darken every 3rd output row for Triad/Dot.
        if (uShadowMask == 1 || uShadowMask == 3) {
            float vrow = mod(floor(outRow), 3.0);
            if (vrow < 1.0) atten *= mix(1.0, 0.6, uShadowStrength);
        }
        rgb *= atten;
    }

    fragColor = vec4(rgb, 1.0);
}
)GLSL";

} // namespace

NtscPostProcessor::NtscPostProcessor() = default;
NtscPostProcessor::~NtscPostProcessor() { destroyGL(); }

bool NtscPostProcessor::initialize()
{
    if (initialized) return ready;
    initialized = true;

#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    if (!loadEntryPoints()) {
        errorMsg = "GL 3.x entry points unavailable";
        return false;
    }
#endif

    program = compileShaderProgram(kVertexShader, kFragmentShader, &errorMsg);
    if (!program) return false;

    uSignal      = glGetUniformLocation(program, "uSignal");
    uPrevFrame   = glGetUniformLocation(program, "uPrev");
    uBrightness  = glGetUniformLocation(program, "uBrightness");
    uContrast    = glGetUniformLocation(program, "uContrast");
    uSaturation  = glGetUniformLocation(program, "uSaturation");
    uHue         = glGetUniformLocation(program, "uHue");
    uSharpness   = glGetUniformLocation(program, "uSharpness");
    uPersistence = glGetUniformLocation(program, "uPersistence");
    uScanlines   = glGetUniformLocation(program, "uScanlines");
    uBarrel      = glGetUniformLocation(program, "uBarrel");
    uSignalSize  = glGetUniformLocation(program, "uSignalSize");
    uShadowMask  = glGetUniformLocation(program, "uShadowMask");
    uShadowStr   = glGetUniformLocation(program, "uShadowStrength");
    uPalMode     = glGetUniformLocation(program, "uPalMode");

    // Fullscreen quad: two triangles covering NDC [-1..1].
    const float verts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
    };
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    ready = true;
    pom2::log().info("NTSC", "OpenEmulator-style composite shader ready");
    return true;
}

bool NtscPostProcessor::createTextures(int sw, int sh)
{
    signalW = sw;
    signalH = sh;
    outW    = sw;          // keep horizontal sample rate
    outH    = sh * 2;      // 2× vertical for scanlines

    // Signal texture (R8) — one byte per 4×fsc sample.
    glGenTextures(1, &signalTex);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
#endif

    // Two output textures + matching FBOs for persistence ping-pong.
    glGenFramebuffers(2, fbo);
    glGenTextures(2, outputTex);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, outputTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, outputTex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            errorMsg = "FBO incomplete";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            // Release everything we allocated so a retry doesn't leak.
            glDeleteFramebuffers(2, fbo);
            glDeleteTextures(2, outputTex);
            glDeleteTextures(1, &signalTex);
            fbo[0] = fbo[1] = 0;
            outputTex[0] = outputTex[1] = 0;
            signalTex = 0;
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void NtscPostProcessor::destroyGL()
{
    // We don't call glDelete* in WASM/dtor — the context is being torn
    // down anyway and unloading isn't guaranteed to be GL-context-safe.
    // (No leak in practice: the context owns the resources.)
    ready = false;
}

unsigned int NtscPostProcessor::process(const uint8_t* signal,
                                        int sw, int sh)
{
    if (!ready) return 0;

    // Lazy texture creation: we need the signal dimensions before we
    // can allocate the FBOs, so the first call sizes everything up.
    if (signalTex == 0) {
        if (!createTextures(sw, sh)) {
            ready = false;
            return 0;
        }
    } else if (sw != signalW || sh != signalH) {
        // Reallocate on dimension change (e.g. someone wired this up
        // for a hypothetical 80-col-only Apple II). Cheap; not on the
        // per-frame path in practice.
        glBindTexture(GL_TEXTURE_2D, signalTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        signalW = sw;
        signalH = sh;
        outW = sw;
        outH = sh * 2;
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, outputTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
    }

    // Upload the new signal frame.
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh,
                    GL_RED, GL_UNSIGNED_BYTE, signal);

    // Save current FBO + viewport + enables so we don't disturb ImGui.
    int prevFbo = 0;
    int prevViewport[4] = {0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);

    const int writeIdx = pingPongIdx;
    const int readIdx  = 1 - pingPongIdx;
    pingPongIdx = readIdx;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo[writeIdx]);
    glViewport(0, 0, outW, outH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glUniform1i(uSignal, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, firstFrame ? signalTex : outputTex[readIdx]);
    glUniform1i(uPrevFrame, 1);

    if (uSignalSize  >= 0) glUniform2f(uSignalSize, float(sw), float(sh));
    if (uBrightness  >= 0) glUniform1f(uBrightness,  params.brightness);
    if (uContrast    >= 0) glUniform1f(uContrast,    params.contrast);
    if (uSaturation  >= 0) glUniform1f(uSaturation,  params.saturation);
    if (uHue         >= 0) glUniform1f(uHue,         params.hue);
    if (uSharpness   >= 0) glUniform1f(uSharpness,   params.sharpness);
    if (uPersistence >= 0) glUniform1f(uPersistence, params.persistence);
    if (uScanlines   >= 0) glUniform1f(uScanlines,   params.scanlines);
    if (uBarrel      >= 0) glUniform1f(uBarrel,      params.barrel);
    if (uShadowMask  >= 0) glUniform1i(uShadowMask,  static_cast<int>(params.shadowMask));
    if (uShadowStr   >= 0) glUniform1f(uShadowStr,   params.shadowMaskStrength);
    if (uPalMode     >= 0) glUniform1i(uPalMode,     params.palMode ? 1 : 0);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Leave neither texture unit holding one of our private textures —
    // the next caller (ImGui or anyone else) shouldn't be able to see
    // them through a stale binding.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore previous FBO + viewport + enables.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1],
               prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    firstFrame = false;
    return outputTex[writeIdx];
}

} // namespace pom2
