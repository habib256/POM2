// POM2 Apple II Emulator
// Copyright (C) 2026

#include "CrtEffectStack.h"
#include "OpenGLShader.h"
#include "Logger.h"

#include <string>

#if defined(__EMSCRIPTEN__)
#  include <GLES3/gl3.h>
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
#  include <GL/gl.h>
#  include <GL/glext.h>
#  include <GLFW/glfw3.h>

// Lazily-loaded GL 2.0+ entry points (Linux/Windows). Same dynamic-loader
// strategy as NtscPostProcessor.cpp / OpenGLShader.cpp — see those for the
// rationale. Kept file-local so the two post-processors stay independent.
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

const char* kVertexShader = R"GLSL(
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Effect-only fragment shader. Input is an already-decoded RGBA framebuffer
// (any colour pipeline); we apply the same post-effects the OE shader does,
// in the same order, so the look is consistent whichever path produced the
// pixels. The demod-specific knobs (sharpness/hue/PAL) don't appear here.
const char* kFragmentShader = R"GLSL(
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uSrc;        // RGBA Apple II framebuffer
uniform sampler2D uPrev;       // previous output (persistence)
uniform vec2  uSrcSize;        // (width, height) of uSrc
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uPersistence;
uniform float uScanlines;
uniform float uBarrel;
uniform int   uShadowMask;     // 0=off,1=triad,2=aperture grille,3=dot
uniform float uShadowStrength; // 0..1

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

    vec3 rgb = texture(uSrc, uv).rgb;

    // ── Brightness / contrast / saturation ────────────────────────
    rgb = (rgb - 0.5) * uContrast + 0.5 + uBrightness;
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(luma), rgb, clamp(uSaturation, 0.0, 4.0));
    rgb = clamp(rgb, 0.0, 1.0);

    // ── Persistence (CRT phosphor decay) ──────────────────────────
    vec3 prev = texture(uPrev, vUv).rgb;
    rgb = max(rgb, prev * clamp(uPersistence, 0.0, 0.98));

    // ── Scanlines (output is 2× vertical of source) ───────────────
    float outRow = uv.y * (uSrcSize.y * 2.0);
    float scan = 1.0 - uScanlines * fract(outRow * 0.5) * 2.0;
    if (mod(floor(outRow), 2.0) < 1.0) scan = 1.0;
    rgb *= scan;

    // ── Shadow mask (procedural, output-pixel space) ──────────────
    if (uShadowMask != 0 && uShadowStrength > 0.0) {
        float ox = uv.x * (uSrcSize.x * 2.0);
        if (uShadowMask == 3) {
            ox += (mod(floor(outRow * 0.5), 2.0) < 1.0) ? 0.0 : 1.5;
        }
        int phase = int(mod(floor(ox), 3.0));
        vec3 maskColor = vec3(0.0);
        if      (phase == 0) maskColor = vec3(1.0, 0.0, 0.0);
        else if (phase == 1) maskColor = vec3(0.0, 1.0, 0.0);
        else                 maskColor = vec3(0.0, 0.0, 1.0);
        vec3 atten = mix(vec3(1.0), maskColor, uShadowStrength);
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

CrtEffectStack::CrtEffectStack() = default;
CrtEffectStack::~CrtEffectStack() = default;

bool CrtEffectStack::initialize()
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

    uSrc         = glGetUniformLocation(program, "uSrc");
    uPrevFrame   = glGetUniformLocation(program, "uPrev");
    uSrcSize     = glGetUniformLocation(program, "uSrcSize");
    uBrightness  = glGetUniformLocation(program, "uBrightness");
    uContrast    = glGetUniformLocation(program, "uContrast");
    uSaturation  = glGetUniformLocation(program, "uSaturation");
    uPersistence = glGetUniformLocation(program, "uPersistence");
    uScanlines   = glGetUniformLocation(program, "uScanlines");
    uBarrel      = glGetUniformLocation(program, "uBarrel");
    uShadowMask  = glGetUniformLocation(program, "uShadowMask");
    uShadowStr   = glGetUniformLocation(program, "uShadowStrength");

    const float verts[] = {
        -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
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
    pom2::log().info("CRT", "Universal CRT effect stack ready");
    return true;
}

bool CrtEffectStack::createTextures(int w, int h)
{
    srcW_ = w;
    srcH_ = h;
    outW  = w;
    outH  = h * 2;   // 2× vertical for scanlines

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
            glDeleteFramebuffers(2, fbo);
            glDeleteTextures(2, outputTex);
            fbo[0] = fbo[1] = 0;
            outputTex[0] = outputTex[1] = 0;
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    firstFrame = true;
    return true;
}

unsigned int CrtEffectStack::process(unsigned int srcTex, int w, int h)
{
    if (!ready || srcTex == 0) return 0;

    if (outputTex[0] == 0) {
        if (!createTextures(w, h)) { ready = false; return 0; }
    } else if (w != srcW_ || h != srcH_) {
        // 80-col toggled the framebuffer width — resize the ping-pong pair.
        srcW_ = w; srcH_ = h; outW = w; outH = h * 2;
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, outputTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        firstFrame = true;
    }

    // Save GL state so we don't disturb ImGui's render.
    int prevFbo = 0, prevViewport[4] = {0};
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
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(uSrc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, firstFrame ? srcTex : outputTex[readIdx]);
    glUniform1i(uPrevFrame, 1);

    if (uSrcSize     >= 0) glUniform2f(uSrcSize, float(w), float(h));
    if (uBrightness  >= 0) glUniform1f(uBrightness,  params.brightness);
    if (uContrast    >= 0) glUniform1f(uContrast,    params.contrast);
    if (uSaturation  >= 0) glUniform1f(uSaturation,  params.saturation);
    if (uPersistence >= 0) glUniform1f(uPersistence, params.persistence);
    if (uScanlines   >= 0) glUniform1f(uScanlines,   params.scanlines);
    if (uBarrel      >= 0) glUniform1f(uBarrel,      params.barrel);
    if (uShadowMask  >= 0) glUniform1i(uShadowMask,  static_cast<int>(params.shadowMask));
    if (uShadowStr   >= 0) glUniform1f(uShadowStr,   params.shadowMaskStrength);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Don't leave our private textures bound on any unit.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    firstFrame = false;
    return outputTex[writeIdx];
}

} // namespace pom2
