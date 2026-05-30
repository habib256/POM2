// Phase-4-final A/B verification tool (NOT in ctest — needs a real GL
// context; run manually on a host with a display / GL driver).
//
// Goal: prove that splitting the OpenEmulator path from a single-pass
// "demod + CRT effects" shader (NtscPostProcessor today) into
//   demod-only (NtscPostProcessor)  →  shared effects (CrtEffectStack)
// produces output equivalent to the current single-pass OE shader, before
// that refactor is committed.
//
// Method: build a deterministic 560×192 composite signal via Apple2Display
// (HGR scene), run the OE post-processing pipeline, glReadPixels the RGBA
// output, and either save it as a baseline or diff against the saved
// baseline — reporting per-config max / mean absolute RGBA delta.
//
//   POM2_BASELINE=1 build/ntsc_oe_ab   # capture pipeline → /tmp/oe_ab_cfgN.raw
//   build/ntsc_oe_ab                    # diff current pipeline vs baseline
//
// Workflow:
//   1. With AB_SPLIT_PIPELINE 0 (current single-pass OE), run BASELINE.
//   2. Refactor NtscPostProcessor → demod-only + route through CrtEffectStack.
//   3. Set AB_SPLIT_PIPELINE 1, rebuild, run (compare) → read the deltas.
//
// A near-zero delta on the "off" config (neutral grading, no geometry) is
// the equivalence proof; non-neutral configs quantify the (expected, small)
// differences from barrel-placement + the 8-bit intermediate texture.

#define AB_SPLIT_PIPELINE 1   // 0 = current single-pass; 1 = demod + CrtEffectStack

#include "Apple2Display.h"
#include "NtscPostProcessor.h"
#include "CrtEffectStack.h"
#include "Memory.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// GL 3.0 FBO entry points (for glReadPixels off the processor's output
// texture). glReadPixels / glGenTextures themselves are core 1.x.
using PFNGENFB   = void (*)(GLsizei, GLuint*);
using PFNBINDFB  = void (*)(GLenum, GLuint);
using PFNFBTEX2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
PFNGENFB   p_glGenFramebuffers      = nullptr;
PFNBINDFB  p_glBindFramebuffer      = nullptr;
PFNFBTEX2D p_glFramebufferTexture2D = nullptr;
#ifndef GL_FRAMEBUFFER
#  define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#  define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

uint8_t patByte(uint32_t a, uint32_t salt)
{
    uint32_t v = a * 2654435761u + salt * 40503u;
    v ^= v >> 15;
    return static_cast<uint8_t>(v & 0xFFu);
}

// Read back the RGBA pixels of texture `tex` (w×h) via a scratch FBO.
std::vector<uint8_t> readTexture(unsigned int tex, int w, int h)
{
    static unsigned int readFbo = 0;
    if (!readFbo) p_glGenFramebuffers(1, &readFbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, readFbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return buf;
}

struct Cfg { const char* name; pom2::NtscParams p; };

std::vector<Cfg> makeConfigs()
{
    std::vector<Cfg> v;
    auto base = [](float scan, float barrel, int mask, float maskStr,
                   float bri, float con, float sat) {
        pom2::NtscParams p;
        p.brightness = bri; p.contrast = con; p.saturation = sat; p.hue = 0.0f;
        p.sharpness = 0.5f;
        p.persistence = 0.0f;          // 0 → no frame-history confound in A/B
        p.scanlines = scan; p.barrel = barrel;
        p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(mask);
        p.shadowMaskStrength = maskStr;
        p.palMode = false; p.textSharp = false;
        return p;
    };
    v.push_back({ "off_neutral",     base(0.0f, 0.0f, 0, 0.0f, 0.0f, 1.0f, 1.0f) });
    v.push_back({ "scanlines_only",  base(0.3f, 0.0f, 0, 0.0f, 0.0f, 1.0f, 1.0f) });
    v.push_back({ "bcs_only",        base(0.0f, 0.0f, 0, 0.0f, 0.05f, 1.1f, 1.2f) });
    v.push_back({ "full",            base(0.3f, 0.05f, 1, 0.5f, 0.0f, 1.0f, 1.0f) });
    return v;
}

} // namespace

int main()
{
    const bool baseline = std::getenv("POM2_BASELINE") != nullptr;

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "ntsc_ab", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "no GL context (need a display)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);

    p_glGenFramebuffers      = reinterpret_cast<PFNGENFB>  (glfwGetProcAddress("glGenFramebuffers"));
    p_glBindFramebuffer      = reinterpret_cast<PFNBINDFB> (glfwGetProcAddress("glBindFramebuffer"));
    p_glFramebufferTexture2D = reinterpret_cast<PFNFBTEX2D>(glfwGetProcAddress("glFramebufferTexture2D"));
    if (!p_glGenFramebuffers || !p_glBindFramebuffer || !p_glFramebufferTexture2D) {
        std::fprintf(stderr, "FBO entry points unavailable\n"); return 1;
    }

    // Deterministic composite signal: a IIe HGR scene through OE mode.
    Memory mem; mem.setIIEMode(true);
    mem.memRead(0xC050); mem.memRead(0xC057);   // graphics + hi-res
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), patByte(a, 4));
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    disp.render(mem);
    if (!disp.signalProduced()) { std::fprintf(stderr, "no signal\n"); return 1; }
    const int sw = disp.signalWidth(), sh = disp.signalHeight();

    pom2::NtscPostProcessor ntsc;
    if (!ntsc.initialize()) { std::fprintf(stderr, "ntsc init failed: %s\n",
                                           ntsc.lastError().c_str()); return 1; }
#if AB_SPLIT_PIPELINE
    pom2::CrtEffectStack crt;
    if (!crt.initialize()) { std::fprintf(stderr, "crt init failed: %s\n",
                                          crt.lastError().c_str()); return 1; }
#endif

    int worst = 0;
    for (const auto& cfg : makeConfigs()) {
        ntsc.setParams(cfg.p);
        unsigned int tex = ntsc.process(disp.signal(), sw, sh);
        int outW = ntsc.outputWidth(), outH = ntsc.outputHeight();
#if AB_SPLIT_PIPELINE
        crt.setParams(cfg.p);
        // Effect pass now renders at an explicit target size; keep the A/B
        // tool's historical 2×-vertical output so its baselines still line up.
        tex = crt.process(tex, ntsc.outputWidth(), ntsc.outputHeight(),
                          ntsc.outputWidth(), ntsc.outputHeight() * 2);
        outW = crt.outputWidth(); outH = crt.outputHeight();
#endif
        if (tex == 0) { std::fprintf(stderr, "process returned 0\n"); return 1; }
        std::vector<uint8_t> got = readTexture(tex, outW, outH);

        const std::string path = "/tmp/oe_ab_" + std::string(cfg.name) + ".raw";
        if (baseline) {
            std::ofstream f(path, std::ios::binary);
            int dims[2] = { outW, outH };
            f.write(reinterpret_cast<const char*>(dims), sizeof(dims));
            f.write(reinterpret_cast<const char*>(got.data()),
                    static_cast<std::streamsize>(got.size()));
            std::printf("  baseline %-16s %dx%d  (%zu bytes)\n",
                        cfg.name, outW, outH, got.size());
        } else {
            std::ifstream f(path, std::ios::binary);
            if (!f) { std::fprintf(stderr, "no baseline %s — run POM2_BASELINE=1 first\n",
                                   path.c_str()); return 1; }
            int dims[2] = {0,0};
            f.read(reinterpret_cast<char*>(dims), sizeof(dims));
            if (dims[0] != outW || dims[1] != outH) {
                std::printf("  %-16s DIM MISMATCH base %dx%d vs now %dx%d\n",
                            cfg.name, dims[0], dims[1], outW, outH);
                worst = 256; continue;
            }
            std::vector<uint8_t> base(got.size());
            f.read(reinterpret_cast<char*>(base.data()),
                   static_cast<std::streamsize>(base.size()));
            long long sum = 0; int maxd = 0, nOver2 = 0;
            for (size_t i = 0; i < got.size(); ++i) {
                int d = std::abs(int(got[i]) - int(base[i]));
                sum += d; if (d > maxd) maxd = d; if (d > 2) ++nOver2;
            }
            const double mean = double(sum) / double(got.size());
            std::printf("  %-16s max=%-3d mean=%.4f  pixels>2LSB=%d / %zu\n",
                        cfg.name, maxd, mean, nOver2, got.size() / 4);
            if (maxd > worst) worst = maxd;
        }
    }

    if (!baseline)
        std::printf("WORST max-delta across configs: %d LSB\n", worst);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
