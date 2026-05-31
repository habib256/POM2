// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Voxel3DRenderer — see Voxel3DRenderer.h. GL plumbing mirrors
// NtscPostProcessor.cpp (same lazy entry-point loader, FBO + state
// save/restore), with two additions the flat 2D passes don't need: a depth
// renderbuffer and instanced drawing.

#include "Voxel3DRenderer.h"

#include "Logger.h"
#include "OpenGLShader.h"

#include <string>

#if defined(__EMSCRIPTEN__)
#  include <GLES3/gl3.h>
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
#  include <GL/gl.h>
#  include <GL/glext.h>
#  include <GLFW/glfw3.h>

// Lazily-loaded GL 2.0+/3.1 entry points (Linux/Windows). Same strategy as
// NtscPostProcessor.cpp / OpenGLShader.cpp.
namespace {
PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers_         = nullptr;
PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer_         = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D_    = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC  glCheckFramebufferStatus_  = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers_      = nullptr;
PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers_        = nullptr;
PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer_        = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage_     = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ = nullptr;
PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers_     = nullptr;
PFNGLGENVERTEXARRAYSPROC         glGenVertexArrays_         = nullptr;
PFNGLBINDVERTEXARRAYPROC         glBindVertexArray_         = nullptr;
PFNGLDELETEVERTEXARRAYSPROC      glDeleteVertexArrays_      = nullptr;
PFNGLGENBUFFERSPROC              glGenBuffers_              = nullptr;
PFNGLBINDBUFFERPROC              glBindBuffer_              = nullptr;
PFNGLBUFFERDATAPROC              glBufferData_              = nullptr;
PFNGLDELETEBUFFERSPROC           glDeleteBuffers_           = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_ = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer_     = nullptr;
PFNGLUSEPROGRAMPROC              glUseProgram_              = nullptr;
PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation_      = nullptr;
PFNGLUNIFORM1IPROC               glUniform1i_               = nullptr;
PFNGLUNIFORM1FPROC               glUniform1f_               = nullptr;
PFNGLUNIFORM2FPROC               glUniform2f_               = nullptr;
PFNGLUNIFORM2IPROC               glUniform2i_               = nullptr;
PFNGLUNIFORM3FPROC               glUniform3f_               = nullptr;
PFNGLUNIFORMMATRIX4FVPROC        glUniformMatrix4fv_        = nullptr;
PFNGLACTIVETEXTUREPROC           glActiveTexture_           = nullptr;
PFNGLDRAWELEMENTSINSTANCEDPROC   glDrawElementsInstanced_   = nullptr;
PFNGLGENERATEMIPMAPPROC          glGenerateMipmap_          = nullptr;
bool entryPointsLoaded_ = false;
bool loadEntryPoints()
{
    if (entryPointsLoaded_) return true;
    auto get = [](const char* n) { return reinterpret_cast<void*>(glfwGetProcAddress(n)); };
#define LOAD(t, v, n) v = reinterpret_cast<t>(get(n))
    LOAD(PFNGLGENFRAMEBUFFERSPROC,         glGenFramebuffers_,         "glGenFramebuffers");
    LOAD(PFNGLBINDFRAMEBUFFERPROC,         glBindFramebuffer_,         "glBindFramebuffer");
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC,    glFramebufferTexture2D_,    "glFramebufferTexture2D");
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC,  glCheckFramebufferStatus_,  "glCheckFramebufferStatus");
    LOAD(PFNGLDELETEFRAMEBUFFERSPROC,      glDeleteFramebuffers_,      "glDeleteFramebuffers");
    LOAD(PFNGLGENRENDERBUFFERSPROC,        glGenRenderbuffers_,        "glGenRenderbuffers");
    LOAD(PFNGLBINDRENDERBUFFERPROC,        glBindRenderbuffer_,        "glBindRenderbuffer");
    LOAD(PFNGLRENDERBUFFERSTORAGEPROC,     glRenderbufferStorage_,     "glRenderbufferStorage");
    LOAD(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer_, "glFramebufferRenderbuffer");
    LOAD(PFNGLDELETERENDERBUFFERSPROC,     glDeleteRenderbuffers_,     "glDeleteRenderbuffers");
    LOAD(PFNGLGENVERTEXARRAYSPROC,         glGenVertexArrays_,         "glGenVertexArrays");
    LOAD(PFNGLBINDVERTEXARRAYPROC,         glBindVertexArray_,         "glBindVertexArray");
    LOAD(PFNGLDELETEVERTEXARRAYSPROC,      glDeleteVertexArrays_,      "glDeleteVertexArrays");
    LOAD(PFNGLGENBUFFERSPROC,              glGenBuffers_,              "glGenBuffers");
    LOAD(PFNGLBINDBUFFERPROC,              glBindBuffer_,              "glBindBuffer");
    LOAD(PFNGLBUFFERDATAPROC,              glBufferData_,              "glBufferData");
    LOAD(PFNGLDELETEBUFFERSPROC,           glDeleteBuffers_,           "glDeleteBuffers");
    LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray_, "glEnableVertexAttribArray");
    LOAD(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer_,     "glVertexAttribPointer");
    LOAD(PFNGLUSEPROGRAMPROC,              glUseProgram_,              "glUseProgram");
    LOAD(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation_,      "glGetUniformLocation");
    LOAD(PFNGLUNIFORM1IPROC,               glUniform1i_,               "glUniform1i");
    LOAD(PFNGLUNIFORM1FPROC,               glUniform1f_,               "glUniform1f");
    LOAD(PFNGLUNIFORM2FPROC,               glUniform2f_,               "glUniform2f");
    LOAD(PFNGLUNIFORM2IPROC,               glUniform2i_,               "glUniform2i");
    LOAD(PFNGLUNIFORM3FPROC,               glUniform3f_,               "glUniform3f");
    LOAD(PFNGLUNIFORMMATRIX4FVPROC,        glUniformMatrix4fv_,        "glUniformMatrix4fv");
    LOAD(PFNGLACTIVETEXTUREPROC,           glActiveTexture_,           "glActiveTexture");
    LOAD(PFNGLDRAWELEMENTSINSTANCEDPROC,   glDrawElementsInstanced_,   "glDrawElementsInstanced");
    LOAD(PFNGLGENERATEMIPMAPPROC,          glGenerateMipmap_,          "glGenerateMipmap");
#undef LOAD
    entryPointsLoaded_ =
        glGenFramebuffers_ && glBindFramebuffer_ && glFramebufferTexture2D_ &&
        glCheckFramebufferStatus_ && glDeleteFramebuffers_ &&
        glGenRenderbuffers_ && glBindRenderbuffer_ && glRenderbufferStorage_ &&
        glFramebufferRenderbuffer_ && glDeleteRenderbuffers_ &&
        glGenVertexArrays_ && glBindVertexArray_ && glDeleteVertexArrays_ &&
        glGenBuffers_ && glBindBuffer_ && glBufferData_ && glDeleteBuffers_ &&
        glEnableVertexAttribArray_ && glVertexAttribPointer_ &&
        glUseProgram_ && glGetUniformLocation_ &&
        glUniform1i_ && glUniform1f_ && glUniform2f_ && glUniform2i_ &&
        glUniform3f_ && glUniformMatrix4fv_ && glActiveTexture_ &&
        glDrawElementsInstanced_ && glGenerateMipmap_;
    return entryPointsLoaded_;
}
} // namespace
#  define glGenFramebuffers         glGenFramebuffers_
#  define glBindFramebuffer         glBindFramebuffer_
#  define glFramebufferTexture2D    glFramebufferTexture2D_
#  define glCheckFramebufferStatus  glCheckFramebufferStatus_
#  define glDeleteFramebuffers      glDeleteFramebuffers_
#  define glGenRenderbuffers        glGenRenderbuffers_
#  define glBindRenderbuffer        glBindRenderbuffer_
#  define glRenderbufferStorage     glRenderbufferStorage_
#  define glFramebufferRenderbuffer glFramebufferRenderbuffer_
#  define glDeleteRenderbuffers     glDeleteRenderbuffers_
#  define glGenVertexArrays         glGenVertexArrays_
#  define glBindVertexArray         glBindVertexArray_
#  define glDeleteVertexArrays      glDeleteVertexArrays_
#  define glGenBuffers              glGenBuffers_
#  define glBindBuffer              glBindBuffer_
#  define glBufferData              glBufferData_
#  define glDeleteBuffers           glDeleteBuffers_
#  define glEnableVertexAttribArray glEnableVertexAttribArray_
#  define glVertexAttribPointer     glVertexAttribPointer_
#  define glUseProgram              glUseProgram_
#  define glGetUniformLocation      glGetUniformLocation_
#  define glUniform1i               glUniform1i_
#  define glUniform1f               glUniform1f_
#  define glUniform2f               glUniform2f_
#  define glUniform2i               glUniform2i_
#  define glUniform3f               glUniform3f_
#  define glUniformMatrix4fv        glUniformMatrix4fv_
#  define glActiveTexture           glActiveTexture_
#  define glDrawElementsInstanced   glDrawElementsInstanced_
#  define glGenerateMipmap          glGenerateMipmap_
#endif

namespace pom2 {

#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
namespace { bool loadEntryPoints() { return true; } }
#endif

namespace {

// Vertex: place a unit cube per instance on the grid; colour comes from a
// vertex texture-fetch of the coloured framebuffer at the cell centre.
//
// MicroM8 "Voxel Cube" model (NOT a luminance height-field): the Apple II
// screen stands UPRIGHT like a monitor (column→world X, row→world Y), and
// every pixel becomes a cube of the *same* thickness extruded toward the
// viewer on +Z ("Voxel Depth"). The optional per-colour forward push
// (uColorShift, MicroM8's "Z-axis 3D offset", 0 = flat slab) is what pops
// colours out — height is never tied to brightness, so no spikes.
const char* kVertexShader = R"GLSL(
in vec3 aPos;            // unit cube: x,y in [-0.5,0.5]; z in [0,1] (back→front)
uniform mat4  uViewProj;
uniform sampler2D uTex;
uniform ivec2 uGrid;       // columns, rows (= live display pixel resolution)
uniform vec2  uCell;       // world cell size (x, y) — sets the 4:3 plane
uniform float uDepth;      // base voxel Z-thickness, in cell-height units
uniform float uFill;       // cube footprint as a fraction of the cell
uniform float uColorShift; // per-colour forward push, in cell-height units (×lum)
out vec3 vWorld;
out vec3 vColor;
void main() {
    int id = gl_InstanceID;
    int cx = id % uGrid.x;
    int cy = id / uGrid.x;
    vec2 uv = (vec2(float(cx) + 0.5, float(cy) + 0.5)) / vec2(uGrid);
    vec4 c = texture(uTex, uv);
    vColor = c.rgb;

    // Upright screen, centred on the origin. Source v=0 is the screen top, so
    // row 0 → world +Y (the gl_Position.y flip below keeps it on-screen-up).
    float wx = (float(cx) + 0.5 - float(uGrid.x) * 0.5) * uCell.x;
    float wy = (float(uGrid.y) * 0.5 - (float(cy) + 0.5)) * uCell.y;

    // Depth + colour push are expressed in CELL units so they stay constant in
    // appearance across resolutions (280 vs 560 wide) instead of turning the
    // voxels into needles. zoff = MicroM8's per-colour "Z-axis 3D offset".
    float lum    = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float depthW = uDepth * uCell.y;
    float zoff   = uColorShift * uCell.y * lum;

    vec3 world = vec3(wx + aPos.x * uCell.x * uFill,
                      wy + aPos.y * uCell.y * uFill,
                      aPos.z * depthW + zoff);
    vWorld = world;
    gl_Position = uViewProj * vec4(world, 1.0);
    // FBO→ImGui::Image present is a vertical mirror (texture row 0 is drawn at
    // the top, but GL renders y-up), exactly like the 2D NtscPostProcessor
    // passes. Pre-flip clip-space Y so the screen reads upright. Cull is off,
    // so the winding swap is harmless.
    gl_Position.y = -gl_Position.y;
}
)GLSL";

// Fragment: per-face flat shading via screen-space derivatives (no normal
// attribute needed — keeps us to the single location-0 "aPos" the shared
// shader helper binds). abs() so back/front winding both light; ambient floor
// keeps faces off pure black.
const char* kFragmentShader = R"GLSL(
in vec3 vWorld;
in vec3 vColor;
uniform vec3  uLightDir;
uniform float uAmbient;
out vec4 fragColor;
void main() {
    vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    float ndl = abs(dot(n, normalize(uLightDir)));
    float shade = uAmbient + (1.0 - uAmbient) * ndl;
    fragColor = vec4(vColor * shade, 1.0);
}
)GLSL";

}  // namespace

Voxel3DRenderer::~Voxel3DRenderer()
{
    if (!ready_) return;
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (depthRb_)  glDeleteRenderbuffers(1, &depthRb_);
    if (fbo_)      glDeleteFramebuffers(1, &fbo_);
    if (ebo_)      glDeleteBuffers(1, &ebo_);
    if (vbo_)      glDeleteBuffers(1, &vbo_);
    if (vao_)      glDeleteVertexArrays(1, &vao_);
}

bool Voxel3DRenderer::initialize()
{
    if (initTried_) return ready_;
    initTried_ = true;

    if (!loadEntryPoints()) {
        pom2::log().warn("Voxel3D", "GL entry points unavailable — 3D view disabled");
        return false;
    }

    std::string err;
    program_ = compileShaderProgram(kVertexShader, kFragmentShader, &err);
    if (!program_) {
        pom2::log().warn("Voxel3D", std::string("shader compile failed: ") + err);
        return false;
    }
    uViewProj_   = glGetUniformLocation(program_, "uViewProj");
    uTex_        = glGetUniformLocation(program_, "uTex");
    uGrid_       = glGetUniformLocation(program_, "uGrid");
    uCell_       = glGetUniformLocation(program_, "uCell");
    uDepth_      = glGetUniformLocation(program_, "uDepth");
    uFill_       = glGetUniformLocation(program_, "uFill");
    uColorShift_ = glGetUniformLocation(program_, "uColorShift");
    uLightDir_   = glGetUniformLocation(program_, "uLightDir");
    uAmbient_    = glGetUniformLocation(program_, "uAmbient");

    // Unit cube: footprint x/y in [-0.5, 0.5]; depth z in [0,1] (back→front).
    static const float kCube[] = {
        -0.5f, -0.5f, 0.0f,   0.5f, -0.5f, 0.0f,   0.5f, 0.5f, 0.0f,   -0.5f, 0.5f, 0.0f,  // back  z=0
        -0.5f, -0.5f, 1.0f,   0.5f, -0.5f, 1.0f,   0.5f, 0.5f, 1.0f,   -0.5f, 0.5f, 1.0f,  // front z=1
    };
    static const unsigned short kIdx[] = {
        0,2,1, 0,3,2,        // back  (-z)
        4,5,6, 4,6,7,        // front (+z)
        0,1,5, 0,5,4,        // bottom (-y)
        3,7,6, 3,6,2,        // top    (+y)
        0,4,7, 0,7,3,        // left   (-x)
        1,2,6, 1,6,5,        // right  (+x)
    };

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCube), kCube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &ebo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIdx), kIdx, GL_STATIC_DRAW);
    glBindVertexArray(0);

    ready_ = true;
    return true;
}

bool Voxel3DRenderer::createTargets(int w, int h)
{
    if (colorTex_ && texW_ == w && texH_ == h) return true;

    if (!colorTex_) glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // The FBO is rendered at the supersample resolution and shown smaller by
    // ImGui, so the minify filter does the anti-alias downsample. Trilinear +
    // a per-frame glGenerateMipmap gives a clean box average (kills the
    // cube-grid moiré); MAG stays LINEAR for the rare upscale.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!depthRb_) glGenRenderbuffers(1, &depthRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    if (!fbo_) glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb_);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        pom2::log().warn("Voxel3D", "framebuffer incomplete");
        return false;
    }
    texW_ = w; texH_ = h;
    return true;
}

unsigned int Voxel3DRenderer::process(unsigned int srcTex, int dstW, int dstH,
                                      const Mat4& viewProj)
{
    if (dstW <= 0 || dstH <= 0) return 0;
    if (!ready_ && !initialize()) return 0;
    if (gridW < 1) gridW = 1;
    if (gridH < 1) gridH = 1;

    // Supersample: render the cube grid into an FBO `ss`× the on-screen size,
    // then let ImGui's minify filter (trilinear, below) box-average it back
    // down — anti-aliases the cube edges and dissolves the moiré without an
    // MSAA resolve. Cap the FBO so a huge window can't blow up VRAM.
    const int ss  = (superSample < 1) ? 1 : (superSample > 4 ? 4 : superSample);
    const int fbW = std::min(dstW * ss, 8192);
    const int fbH = std::min(dstH * ss, 8192);
    if (!createTargets(fbW, fbH)) return 0;

    // Save GL state we touch (mirrors NtscPostProcessor's dance, + depth).
    GLint prevFbo = 0, prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fbW, fbH);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(bg[0], bg[1], bg[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(uTex_, 0);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, viewProj.m);
    glUniform2i(uGrid_, gridW, gridH);
    // Plane is a true 4:3 monitor: width 2.0, height 1.5, centred on the origin.
    // Cells follow the grid so the voxels keep the Apple II pixel shape.
    glUniform2f(uCell_, 2.0f / static_cast<float>(gridW),
                        1.5f / static_cast<float>(gridH));
    glUniform1f(uDepth_, voxelDepth);
    glUniform1f(uFill_, cubeFill);
    glUniform1f(uColorShift_, colorShift);
    glUniform3f(uLightDir_, 0.35f, 0.5f, 0.8f);   // upper-front key light
    glUniform1f(uAmbient_, ambient);

    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr, gridW * gridH);
    glBindVertexArray(0);

    // Build the mip chain so the trilinear minify downsample (ImGui draws this
    // ss× texture at 1× size) has a proper box-filtered level to sample.
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Restore.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    return colorTex_;
}

}  // namespace pom2
