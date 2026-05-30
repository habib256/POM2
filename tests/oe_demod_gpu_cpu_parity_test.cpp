// Pins OpenEmulator demod parity: the GPU shader phase formula must match
// renderCompositeOeCpu() (the reference CPU path). Without GL — simulates
// the fixed GLSL demod in C++ and compares pixel-for-pixel on HGR + DHGR.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int   N = 8;

static const float kLumaK[N + 1] = {
    0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
    -0.02210f, -0.00999f, -0.00072f, 0.00130f
};
static const float kChromaSoft[N + 1] = {
    0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
    0.07008f, 0.03648f, 0.01543f, 0.00515f
};

uint32_t packRgb(float r, float g, float b)
{
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const uint32_t R = static_cast<uint32_t>(cl(r) * 255.0f + 0.5f);
    const uint32_t G = static_cast<uint32_t>(cl(g) * 255.0f + 0.5f);
    const uint32_t B = static_cast<uint32_t>(cl(b) * 255.0f + 0.5f);
    return 0xFF000000u | (B << 16) | (G << 8) | R;
}

// GPU shader demod (post-fix): same phase lookup as renderCompositeOeCpu().
uint32_t demodGpuStyle(const uint8_t* row, int x, int phaseOffset)
{
    float Y = 0.0f, U = 0.0f, V = 0.0f;
    const float sigX = static_cast<float>(x) + 0.5f;
    for (int i = -N; i <= N; ++i) {
        const float fx = sigX + static_cast<float>(i);
        const int   xi = static_cast<int>(std::floor(fx));
        float s = 0.0f;
        if (xi >= 0 && xi < 560)
            s = row[xi] ? 1.0f : 0.0f;
        const int a = i < 0 ? -i : i;
        const int k = (xi + phaseOffset) & 3;
        const int phaseIdx = (k + phaseOffset) & 3;
        const float phase = kPi * 0.5f * static_cast<float>(phaseIdx);
        Y += s * kLumaK[a];
        U += s * std::sin(phase) * kChromaSoft[a];
        V += s * std::cos(phase) * kChromaSoft[a];
    }
    const float r = Y + 1.139883f * V;
    const float g = Y - 0.394642f * U - 0.580622f * V;
    const float b = Y + 2.032062f * U;
    return packRgb(r, g, b);
}

uint32_t pxCpu(const Apple2Display& d, int x, int y)
{
    return d.pixels()[y * d.width() + x] & 0x00FFFFFFu;
}

int maxChannelDelta(uint32_t a, uint32_t b)
{
    const int dr = static_cast<int>((a      ) & 0xFF) - static_cast<int>((b      ) & 0xFF);
    const int dg = static_cast<int>((a >> 8 ) & 0xFF) - static_cast<int>((b >> 8 ) & 0xFF);
    const int db = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
    return std::max({std::abs(dr), std::abs(dg), std::abs(db)});
}

void assertParity(const Apple2Display& disp, int phaseOffset)
{
    const int w = disp.width();
    const int h = disp.height();
    const uint8_t* sig = disp.signal();
    int worst = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = sig + static_cast<size_t>(y) * 560;
        for (int x = 0; x < w; ++x) {
            const uint32_t cpuPx = pxCpu(disp, x, y);
            const uint32_t gpuPx = demodGpuStyle(row, x, phaseOffset);
            const int d = maxChannelDelta(cpuPx, gpuPx);
            if (d > worst) worst = d;
            assert(d <= 1 && "GPU-style demod must match OE CPU within 1 LSB");
        }
    }
    std::printf("  phaseOffset=%d maxDelta=%d\n", phaseOffset, worst);
}

void setupHgr(Memory& mem)
{
    mem.memRead(0xC050);
    mem.memRead(0xC057);
    const uint16_t row = 0x2000;
    mem.memWrite(row, 0x55);
    mem.memWrite(static_cast<uint16_t>(row + 1), 0x2A);
    mem.memWrite(static_cast<uint16_t>(row + 2), 0xD5);
}

void setupDhgr(Memory& mem)
{
    mem.setIIEMode(true);
    mem.memRead(0xC050);
    mem.memWrite(0xC00D, 0);
    mem.memRead(0xC057);
    mem.memRead(0xC05E);
    const uint16_t row = 0x2000;
    mem.memWrite(row, 0x55);
    mem.auxDataMutable()[row] = 0x2A;
}

} // namespace

int main()
{
    Apple2Display disp;
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);

    Memory memHgr;
    setupHgr(memHgr);
    disp.render(memHgr);
    assert(disp.signalProduced());
    assert(disp.signalPhaseOffset() == 0);
    std::printf("HGR parity:\n");
    assertParity(disp, 0);

    Memory memDhgr;
    setupDhgr(memDhgr);
    disp.setAuxMemory(memDhgr.auxData());
    disp.render(memDhgr);
    assert(disp.signalPhaseOffset() == 1);
    std::printf("DHGR parity:\n");
    assertParity(disp, 1);

    std::printf("oe_demod_gpu_cpu_parity OK\n");
    return 0;
}
