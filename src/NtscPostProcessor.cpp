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

// Fragment shader: NTSC composite DEMODULATION only (Phase 4 final).
//
// This pass now recovers colour from the composite signal and nothing
// else — barrel geometry, brightness/contrast/saturation, phosphor
// persistence, scanlines and the shadow mask all moved to the shared
// CrtEffectStack so OE and every other colour mode go through ONE effects
// implementation. MainWindow chains this demod into CrtEffectStack.
//
// Pipeline per output fragment:
//   1. Sample N taps around the current column of the signal texture.
//      Y is a narrow-gaussian sum (sharp luma); chroma multiplies each
//      tap by sin/cos of the 4×fsc subcarrier phase (π/2 per dot — the
//      Apple II pixel clock IS the colorburst) under a WIDER gaussian
//      whose width the sharpness slider sets (chroma bandwidth).
//   2. Hue rotation in the I/Q plane, then the standard NTSC YIQ→RGB
//      matrix. PAL line-phase alternation flips the Q sign on odd lines.
//   3. Output the demodulated RGB (clamped). Grading + CRT glass happen
//      downstream in CrtEffectStack. Output is 1× (no scanline doubling).
const char* kFragmentShader = R"GLSL(
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uSignal;
uniform vec2  uSignalSize;     // (width, height) of the signal texture
uniform float uHue;
uniform float uSharpness;
uniform int   uPalMode;        // 0=NTSC, 1=PAL (line-phase alternation)

const float PI = 3.14159265358979;

float sampleSignal(float x, float y)
{
    if (x < 0.0 || x >= uSignalSize.x || y < 0.0 || y >= uSignalSize.y) return 0.0;
    return texture(uSignal, vec2(x / uSignalSize.x, y / uSignalSize.y)).r;
}

void main()
{
    float sigX = vUv.x * uSignalSize.x;
    float sigY = vUv.y * uSignalSize.y;

    // ── PAL line-phase alternation ────────────────────────────────
    float palQSign = 1.0;
    if (uPalMode == 1) {
        float lineIdx = floor(sigY);
        palQSign = (mod(lineIdx, 2.0) < 1.0) ? 1.0 : -1.0;
    }

    // ── Y / I / Q accumulation over a small kernel ────────────────
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

        // 4×-fsc: one subcarrier cycle per 4 dots. The +1.5π (≡ −90°)
        // term aligns the demodulated I/Q axes with the Apple II colorburst
        // phase so artifact hues match the MAME LUT reference (ColorNTSC):
        // without it the wheel was rotated 90° (green→blue, magenta→orange).
        // Calibrated against the Total Replay HGR splash across 0/90/180/270.
        float phase = PI * 0.5 * fx + PI * 1.5;
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
    rgb = clamp(rgb, 0.0, 1.0);
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
    uSignalSize  = glGetUniformLocation(program, "uSignalSize");
    uHue         = glGetUniformLocation(program, "uHue");
    uSharpness   = glGetUniformLocation(program, "uSharpness");
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
    outH    = sh;          // demod-only is 1×; CrtEffectStack does the 2×

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

    // Single demod output texture + FBO. NEAREST: this is the demod
    // intermediate that CrtEffectStack samples; nearest keeps its texels
    // clean (the effect pass does its own filtering / 2× scanline expansion).
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &outputTex);
    glBindTexture(GL_TEXTURE_2D, outputTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, outputTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        errorMsg = "FBO incomplete";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &outputTex);
        glDeleteTextures(1, &signalTex);
        fbo = 0; outputTex = 0; signalTex = 0;
        return false;
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
        outH = sh;
        glBindTexture(GL_TEXTURE_2D, outputTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, outW, outH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glUniform1i(uSignal, 0);

    // Demod-only uniforms (CRT glass lives in CrtEffectStack now).
    if (uSignalSize >= 0) glUniform2f(uSignalSize, float(sw), float(sh));
    if (uHue        >= 0) glUniform1f(uHue,       params.hue);
    if (uSharpness  >= 0) glUniform1f(uSharpness, params.sharpness);
    if (uPalMode    >= 0) glUniform1i(uPalMode,   params.palMode ? 1 : 0);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Leave no private texture bound on unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore previous FBO + viewport + enables.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1],
               prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    return outputTex;
}

} // namespace pom2
