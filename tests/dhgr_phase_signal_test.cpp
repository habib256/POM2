// DHGR NTSC phase-offset smoke test — pins signalPhaseOffset() and verifies
// ColorCompositeOECpu applies the +1 subcarrier shift (MAME rotl4 absX+1).

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t CLR_TEXT     = 0xC050;
constexpr uint16_t SET_HIRES    = 0xC057;
constexpr uint16_t DHIRES_ON    = 0xC05E;

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

uint32_t px(const Apple2Display& d, int x, int y)
{
    const uint32_t* fb = d.pixels();
    return fb[y * d.width() + x] & 0x00FFFFFFu;
}

uint32_t demodAnchor(const uint8_t* row, int x, int phaseOffset)
{
    constexpr float kPi = 3.14159265358979f;
    constexpr int   N = 8;
    static const float lumaK[N + 1] = {
        0.27941f, 0.23593f, 0.13462f, 0.03665f, -0.01538f,
        -0.02210f, -0.00999f, -0.00072f, 0.00130f
    };
    static const float chromaK[N + 1] = {
        0.26030f, 0.24788f, 0.21373f, 0.16602f, 0.11509f,
        0.07008f, 0.03648f, 0.01543f, 0.00515f
    };
    float sinP[4], cosP[4];
    for (int k = 0; k < 4; ++k) {
        const float ph = kPi * 0.5f * static_cast<float>((k + phaseOffset) & 3);
        sinP[k] = std::sin(ph);
        cosP[k] = std::cos(ph);
    }
    float Y = 0.0f, U = 0.0f, V = 0.0f;
    for (int i = -N; i <= N; ++i) {
        const int xi = x + i;
        if (xi < 0 || xi >= 560) continue;
        const float s = row[xi] ? 1.0f : 0.0f;
        const int   k = (xi + phaseOffset) & 3;
        const int   a = i < 0 ? -i : i;
        Y += s * lumaK[a];
        U += s * sinP[k] * chromaK[a];
        V += s * cosP[k] * chromaK[a];
    }
    float r = Y + 1.139883f * V;
    float g = Y - 0.394642f * U - 0.580622f * V;
    float b = Y + 2.032062f * U;
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const uint32_t R = static_cast<uint32_t>(cl(r) * 255.0f + 0.5f);
    const uint32_t G = static_cast<uint32_t>(cl(g) * 255.0f + 0.5f);
    const uint32_t B = static_cast<uint32_t>(cl(b) * 255.0f + 0.5f);
    return (R << 0) | (G << 8) | (B << 16);
}

void setupDhgrPattern(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(SET_HIRES);
    mem.memRead(DHIRES_ON);

    const uint16_t row = hgrAddr(0);
    mem.memWrite(row, 0x55);
    mem.auxDataMutable()[row] = 0x2A;
}

} // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    setupDhgrPattern(mem);

    Apple2Display oe;
    oe.setAuxMemory(mem.auxData());
    oe.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    oe.render(mem);
    assert(oe.signalPhaseOffset() == 1 && "DHGR must use +1 NTSC phase offset");
    assert(oe.signalProduced());

    const uint32_t oePx = px(oe, 14, 0);
    const uint8_t* sigRow = oe.signal();
    const uint32_t ph0 = demodAnchor(sigRow, 14, 0);
    const uint32_t ph1 = demodAnchor(sigRow, 14, 1);
    assert(ph0 != ph1 && "phase offset must change the demodulated colour");
    assert(oePx == ph1 && "OE CPU path must demod with DHGR phase offset +1");
    assert(oePx != ph0 && "OE CPU must not use HGR phase on a DHGR signal");

    Memory memHgr;
    memHgr.setIIEMode(true);
    memHgr.memRead(CLR_TEXT);
    memHgr.memRead(SET_HIRES);
    memHgr.memWrite(hgrAddr(0), 0x55);

    Apple2Display oeHgr;
    oeHgr.setAuxMemory(memHgr.auxData());
    oeHgr.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    oeHgr.render(memHgr);
    assert(oeHgr.signalPhaseOffset() == 0 && "HGR must not apply DHGR phase offset");

    std::printf("dhgr_phase_signal OK\n");
    return 0;
}
