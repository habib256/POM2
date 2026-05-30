// Diagnostic (EXCLUDE_FROM_ALL, GL): feed a dumped composite signal to the
// REAL NtscPostProcessor GLSL shader and write its demodulated output as a
// PPM. Compares each pixel against the OpenEmulator-exact FIR YUV CPU demod
// (same math as renderCompositeOeCpu / oe_demod_gpu_cpu_parity).
//
//   build/oe_signal_view <signal.bin> <out.ppm> [phaseOffset]
//
// signal.bin = 560×192 R8 (0x00/0xFF). phaseOffset: 0=HGR/text (default),
// 1=DHGR (MAME rotl4 absX+1).

#include "NtscPostProcessor.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int   N = 8;

static const float kLumaK[N + 1] = {
    0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
    -0.02210f, -0.00999f, -0.00072f, 0.00130f
};
static const float kChromaK[N + 1] = {
    0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
    0.07008f, 0.03648f, 0.01543f, 0.00515f
};

using PFNGENFB   = void (*)(GLsizei, GLuint*);
using PFNBINDFB  = void (*)(GLenum, GLuint);
using PFNFBTEX2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
PFNGENFB p_gen = nullptr;
PFNBINDFB p_bind = nullptr;
PFNFBTEX2D p_tex = nullptr;

#ifndef GL_FRAMEBUFFER
#  define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#  define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

void demodCpuFir(const uint8_t* row, int sw, int x, int phaseOffset,
                 int& R, int& G, int& B)
{
    float sinP[4], cosP[4];
    for (int k = 0; k < 4; ++k) {
        const float ph = kPi * 0.5f * static_cast<float>((k + phaseOffset) & 3);
        sinP[k] = std::sin(ph);
        cosP[k] = std::cos(ph);
    }
    float Y = 0.0f, U = 0.0f, V = 0.0f;
    for (int i = -N; i <= N; ++i) {
        const int xi = x + i;
        if (xi < 0 || xi >= sw) continue;
        const float s = row[xi] ? 1.0f : 0.0f;
        const int   k = (xi + phaseOffset) & 3;
        const int   a = i < 0 ? -i : i;
        Y += s * kLumaK[a];
        U += s * sinP[k] * kChromaK[a];
        V += s * cosP[k] * kChromaK[a];
    }
    const float r = Y + 1.139883f * V;
    const float g = Y - 0.394642f * U - 0.580622f * V;
    const float b = Y + 2.032062f * U;
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    R = static_cast<int>(cl(r) * 255.0f + 0.5f);
    G = static_cast<int>(cl(g) * 255.0f + 0.5f);
    B = static_cast<int>(cl(b) * 255.0f + 0.5f);
}

int channelDelta(int a, int b) { return std::abs(a - b); }

int rgbDelta(int cr, int cg, int cb, const uint8_t* px)
{
    return std::max({channelDelta(cr, px[0]),
                     channelDelta(cg, px[1]),
                     channelDelta(cb, px[2])});
}

} // namespace

int main(int argc, char** argv)
{
    const char* sigPath = argc > 1 ? argv[1] : "/tmp/modes6/oe_signal.bin";
    const char* outPath = argc > 2 ? argv[2] : "/tmp/oe_real.ppm";
    const int   phaseOffset = argc > 3 ? std::atoi(argv[3]) : 0;
    const int sw = 560, sh = 192;

    std::ifstream f(sigPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "no signal %s\n", sigPath);
        return 1;
    }
    std::vector<uint8_t> sig(static_cast<size_t>(sw) * sh);
    f.read(reinterpret_cast<char*>(sig.data()), static_cast<std::streamsize>(sig.size()));

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "oe_signal_view", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "no GL context\n");
        return 1;
    }
    glfwMakeContextCurrent(win);
    p_gen  = reinterpret_cast<PFNGENFB>(glfwGetProcAddress("glGenFramebuffers"));
    p_bind = reinterpret_cast<PFNBINDFB>(glfwGetProcAddress("glBindFramebuffer"));
    p_tex  = reinterpret_cast<PFNFBTEX2D>(glfwGetProcAddress("glFramebufferTexture2D"));

    pom2::NtscPostProcessor ntsc;
    pom2::NtscParams params;
    params.hue       = 0.0f;
    params.sharpness = 0.5f;
    params.palMode   = false;
    ntsc.setParams(params);
    if (!ntsc.initialize()) {
        std::fprintf(stderr, "NtscPostProcessor init: %s\n", ntsc.lastError().c_str());
        return 1;
    }
    const unsigned tex = ntsc.process(sig.data(), sw, sh, phaseOffset);
    const int ow = ntsc.outputWidth(), oh = ntsc.outputHeight();

    unsigned fbo = 0;
    p_gen(1, &fbo);
    p_bind(GL_FRAMEBUFFER, fbo);
    p_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> rgba(static_cast<size_t>(ow) * oh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // Spot-check artifact bands + full-frame max delta (FIR CPU vs GLSL).
    const int bandRow[4] = {24, 72, 120, 168};
    const char* bn[4]    = {"violet", "green ", "blue  ", "orange"};
    int maxDelta = 0;
    long long sumDelta = 0;
    int samples = 0;

    std::fprintf(stderr,
        "phaseOffset=%d  (0=HGR/text, 1=DHGR)\n"
        "band     FIR CPU demod      GLSL readback   delta\n",
        phaseOffset);
    for (int bi = 0; bi < 4; ++bi) {
        const int y = bandRow[bi];
        const int x = 280;
        int cr = 0, cg = 0, cb = 0;
        demodCpuFir(sig.data() + static_cast<size_t>(y) * sw, sw, x, phaseOffset,
                    cr, cg, cb);
        const uint8_t* px = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
        const int d = rgbDelta(cr, cg, cb, px);
        maxDelta = std::max(maxDelta, d);
        std::fprintf(stderr, "  %s (%3d,%3d,%3d)   (%3d,%3d,%3d)   %d\n",
                     bn[bi], cr, cg, cb, px[0], px[1], px[2], d);
    }

    for (int y = 0; y < oh; ++y) {
        const uint8_t* row = sig.data() + static_cast<size_t>(y) * sw;
        for (int x = 0; x < ow; ++x) {
            int cr = 0, cg = 0, cb = 0;
            demodCpuFir(row, sw, x, phaseOffset, cr, cg, cb);
            const uint8_t* px = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
            const int d = rgbDelta(cr, cg, cb, px);
            maxDelta = std::max(maxDelta, d);
            sumDelta += d;
            ++samples;
        }
    }
    const double meanDelta = samples ? static_cast<double>(sumDelta) / samples : 0.0;
    std::fprintf(stderr, "full frame: maxDelta=%d meanDelta=%.3f (%dx%d)\n",
                 maxDelta, meanDelta, ow, oh);

    std::ofstream o(outPath, std::ios::binary);
    o << "P6\n" << ow << " " << oh << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(ow) * oh * 3);
    for (int i = 0; i < ow * oh; ++i) {
        rgb[static_cast<size_t>(i) * 3 + 0] = rgba[static_cast<size_t>(i) * 4 + 0];
        rgb[static_cast<size_t>(i) * 3 + 1] = rgba[static_cast<size_t>(i) * 4 + 1];
        rgb[static_cast<size_t>(i) * 3 + 2] = rgba[static_cast<size_t>(i) * 4 + 2];
    }
    o.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
    std::fprintf(stderr, "wrote %s (%dx%d) — GLSL OE demod (FIR reference)\n",
                 outPath, ow, oh);

    glfwDestroyWindow(win);
    glfwTerminate();
    return maxDelta > 2 ? 2 : 0;   // warn if GPU diverges >2 LSB from FIR CPU
}
