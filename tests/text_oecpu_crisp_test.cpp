// TEXT under ColorCompositeOECpu must be crisp mono (renderInternal), not
// NTSC-demodded from the composite signal — same pixels as ColorNTSC.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint16_t SET_TEXT = 0xC051;

void fillMain(Memory& m, uint16_t base, uint16_t end, uint8_t v)
{
    for (uint16_t a = base; a < end; ++a)
        m.memWrite(a, v);
}

} // namespace

int main()
{
    Memory mem;
    mem.memRead(SET_TEXT);
    fillMain(mem, 0x0400, 0x0800, static_cast<uint8_t>('A'));

    Apple2Display ntsc;
    Apple2Display oecpu;
    ntsc.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    oecpu.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);

    ntsc.render(mem);
    oecpu.render(mem);

    assert(ntsc.width() == oecpu.width());
    assert(ntsc.height() == oecpu.height());
    assert(oecpu.signalProduced() && "OE CPU still serialises TEXT for tools");

    const int n = ntsc.width() * ntsc.height();
    const uint32_t* a = ntsc.pixels();
    const uint32_t* b = oecpu.pixels();
    for (int i = 0; i < n; ++i) {
        assert((a[i] & 0x00FFFFFFu) == (b[i] & 0x00FFFFFFu)
               && "TEXT OE-CPU must match crisp LUT path");
    }

    std::printf("text_oecpu_crisp OK\n");
    return 0;
}
