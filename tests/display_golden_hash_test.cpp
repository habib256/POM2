// Display golden-hash oracle — Phase 0 regression net for the display
// reorganization (CLAUDE.md "layers d'effets" refactor).
//
// Purpose: freeze the EXACT current output of every CPU decode path so a
// mechanical refactor (factoring shared decoders, splitting HiResMode into
// pipeline+effect layers) can be proven byte-for-byte behaviour-preserving.
// This is the oracle the refactor leans on — if a hash changes unexpectedly
// during Phases 1-2, a decoder was altered, not just moved.
//
// What it covers: the cross product of
//   scenes  = {text40, text80, lores, lores+mixed, hgr, hgr+mixed,
//              dhgr, dhgr+mixed}  on a //e, plus {text40, lores, hgr} on a
//              ][+ (the isIIE()==false branches).
//   modes   = the integer-deterministic colour pipelines:
//              ColorNTSC, ColorCompMedium, ColorComp4Bit, ChatMauveRGB,
//              MonoWhite, MonoGreen, MonoAmber.
//   signal  = the 14.318 MHz composite generator (fillCompositeSignal),
//              hashed once per scene under ColorCompositeOE.
//
// Deliberately EXCLUDED from the golden table: ColorAppleWin's framebuffer.
// Its chromaLut[4][4096] is built from floating-point IIR math at init, so
// the final RGBA8 quantization is host/compiler-FP-dependent and would make
// a baked hash flaky in CI. The AppleWin path is covered two other ways:
//   - its *input* (the composite signal) is golden-hashed here, and
//   - its decode is pinned by applewin_ntsc_smoke_test.
//
// Determinism: every (scene, mode) pair renders into a FRESH Apple2Display,
// so frameCounter==1 (flash phase 0), persistence/AppleWin history are clear,
// and iteration order is irrelevant. All hashed outputs are pure integer
// math (LUTs, palettes, bit ops) → stable across platforms.
//
// Regenerating goldens (after an INTENTIONAL decode change):
//   POM2_GOLDEN_RECORD=1 build/test_display_golden
// then paste the printed table over kGolden[] below.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ── FNV-1a 64-bit over a raw byte span ───────────────────────────────────
uint64_t fnv1a(const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628257ull;
    }
    return h;
}

// ── Soft-switch addresses (mirror dhgr_render_smoke_test) ─────────────────
constexpr uint16_t IIE_80COL_OFF = 0xC00C;
constexpr uint16_t IIE_80COL_ON  = 0xC00D;
constexpr uint16_t SET_TEXT      = 0xC051;
constexpr uint16_t CLR_TEXT      = 0xC050;
constexpr uint16_t SET_MIXED     = 0xC053;
constexpr uint16_t CLR_MIXED     = 0xC052;
constexpr uint16_t SET_PAGE1     = 0xC054;
constexpr uint16_t SET_HIRES     = 0xC057;
constexpr uint16_t CLR_HIRES     = 0xC056;
constexpr uint16_t DHIRES_ON     = 0xC05E;
constexpr uint16_t DHIRES_OFF    = 0xC05F;

// Deterministic, content-rich pattern for a memory address. Spreads bits
// across the byte so HGR artifact decode, lo-res nibbles and text glyph
// ranges (normal/inverse/flash) all get exercised.
uint8_t patByte(uint32_t addr, uint32_t salt)
{
    uint32_t v = addr * 2654435761u + salt * 40503u;
    v ^= v >> 15;
    return static_cast<uint8_t>(v & 0xFFu);
}

void fillMain(Memory& mem, uint16_t lo, uint16_t hi, uint32_t salt)
{
    for (uint32_t a = lo; a < hi; ++a)
        mem.memWrite(static_cast<uint16_t>(a), patByte(a, salt));
}

void fillAux(Memory& mem, uint16_t lo, uint16_t hi, uint32_t salt)
{
    uint8_t* aux = mem.auxDataMutable();
    for (uint32_t a = lo; a < hi; ++a)
        aux[a] = patByte(a, salt);
}

// Bring every relevant soft switch to a known baseline, then let each scene
// flip only what it needs. NOTE: the $C00x IIe paging switches (80COL) only
// respond to WRITES on a //e (Memory.cpp:892 gates iieHandleSoftSwitch on
// isWrite); the $C05x display switches respond to reads. Drive each via its
// real access type or the soft switch silently no-ops.
void baseline(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memRead(CLR_MIXED);
    mem.memRead(SET_PAGE1);
    mem.memRead(CLR_HIRES);
    mem.memWrite(IIE_80COL_OFF, 0);
    mem.memRead(DHIRES_OFF);
}

struct Scene {
    const char* name;
    bool        iie;
    void      (*setup)(Memory&);
};

void sText40(Memory& m) {
    baseline(m); m.memRead(SET_TEXT);
    fillMain(m, 0x0400, 0x0800, 1);
}
void sText80(Memory& m) {
    baseline(m); m.memRead(SET_TEXT); m.memWrite(IIE_80COL_ON, 0);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
// 40-col TEXT with DHIRES(AN3) on + 80COL off: triggers the Le Chat Mauve
// foreground/background colour-text path (renderTextChatMauveFgBg) under the
// ChatMauveRGB mode (char from main, fg/bg attr from aux). Other modes just
// render plain 40-col mono text — only the chatmauve hash exercises the path.
void sTextColorCM(Memory& m) {
    baseline(m); m.memRead(SET_TEXT); m.memRead(DHIRES_ON);   // 80COL stays off
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}
void sLoRes(Memory& m) {
    baseline(m);                       // graphics + lo-res
    fillMain(m, 0x0400, 0x0800, 3);
}
void sLoResMixed(Memory& m) {
    baseline(m); m.memRead(SET_MIXED);
    fillMain(m, 0x0400, 0x0800, 3);
}
void sHgr(Memory& m) {
    baseline(m); m.memRead(SET_HIRES);
    fillMain(m, 0x2000, 0x4000, 4);
}
void sHgrMixed(Memory& m) {
    baseline(m); m.memRead(SET_HIRES); m.memRead(SET_MIXED);
    fillMain(m, 0x2000, 0x4000, 4);
    fillMain(m, 0x0400, 0x0800, 1);    // bottom 4 text rows
}
void sDhgr(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(SET_HIRES); m.memRead(DHIRES_ON);
    fillMain(m, 0x2000, 0x4000, 4); fillAux(m, 0x2000, 0x4000, 5);
}
void sDhgrMixed(Memory& m) {
    baseline(m); m.memWrite(IIE_80COL_ON, 0); m.memRead(SET_HIRES);
    m.memRead(DHIRES_ON); m.memRead(SET_MIXED);
    fillMain(m, 0x2000, 0x4000, 4); fillAux(m, 0x2000, 0x4000, 5);
    fillMain(m, 0x0400, 0x0800, 1); fillAux(m, 0x0400, 0x0800, 2);
}

const Scene kScenes[] = {
    { "iie/text40",     true,  sText40    },
    { "iie/text80",     true,  sText80    },
    { "iie/textcolorcm",true,  sTextColorCM},
    { "iie/lores",      true,  sLoRes     },
    { "iie/loresmixed", true,  sLoResMixed},
    { "iie/hgr",        true,  sHgr       },
    { "iie/hgrmixed",   true,  sHgrMixed  },
    { "iie/dhgr",       true,  sDhgr      },
    { "iie/dhgrmixed",  true,  sDhgrMixed },
    { "ii+/text40",     false, sText40    },
    { "ii+/lores",      false, sLoRes     },
    { "ii+/hgr",        false, sHgr       },
};

struct ModeEntry {
    Apple2Display::HiResMode m;
    const char* tag;
};
const ModeEntry kModes[] = {
    { Apple2Display::HiResMode::ColorNTSC,       "ntsc"     },
    { Apple2Display::HiResMode::ColorCompMedium, "medium"   },
    { Apple2Display::HiResMode::ColorComp4Bit,   "4bit"     },
    { Apple2Display::HiResMode::ChatMauveRGB,    "chatmauve"},
    { Apple2Display::HiResMode::MonoWhite,       "monowhite"},
    { Apple2Display::HiResMode::MonoGreen,       "monogreen"},
    { Apple2Display::HiResMode::MonoAmber,       "monoamber"},
};

// ── Golden table (host-independent integer paths) ─────────────────────────
// Populated by POM2_GOLDEN_RECORD=1 — see file header.
const std::map<std::string, uint64_t> kGolden = {
    { "iie/text40/ntsc", 0x64b9ef4cc731c75cULL },
    { "iie/text40/medium", 0x64b9ef4cc731c75cULL },
    { "iie/text40/4bit", 0x64b9ef4cc731c75cULL },
    { "iie/text40/chatmauve", 0x64b9ef4cc731c75cULL },
    { "iie/text40/monowhite", 0x64b9ef4cc731c75cULL },
    { "iie/text40/monogreen", 0x64b9ef4cc731c75cULL },
    { "iie/text40/monoamber", 0x64b9ef4cc731c75cULL },
    { "iie/text40/signal", 0x3643a247208629e3ULL },
    { "iie/text80/ntsc", 0x355e30039c76795cULL },
    { "iie/text80/medium", 0x355e30039c76795cULL },
    { "iie/text80/4bit", 0x355e30039c76795cULL },
    { "iie/text80/chatmauve", 0x355e30039c76795cULL },
    { "iie/text80/monowhite", 0x355e30039c76795cULL },
    { "iie/text80/monogreen", 0x355e30039c76795cULL },
    { "iie/text80/monoamber", 0x355e30039c76795cULL },
    { "iie/text80/signal", 0x7e66d99b2bb925fcULL },
    { "iie/textcolorcm/ntsc", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/medium", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/4bit", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/chatmauve", 0xcb80c0ac9bb5a603ULL },
    { "iie/textcolorcm/monowhite", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/monogreen", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/monoamber", 0x64b9ef4cc731c75cULL },
    { "iie/textcolorcm/signal", 0x3643a247208629e3ULL },
    { "iie/lores/ntsc", 0x85abcadd1bb83d83ULL },
    { "iie/lores/medium", 0x85abcadd1bb83d83ULL },
    { "iie/lores/4bit", 0x85abcadd1bb83d83ULL },
    { "iie/lores/chatmauve", 0xa78a672e2f668d03ULL },
    { "iie/lores/monowhite", 0x85abcadd1bb83d83ULL },
    { "iie/lores/monogreen", 0x85abcadd1bb83d83ULL },
    { "iie/lores/monoamber", 0x85abcadd1bb83d83ULL },
    { "iie/lores/signal", 0x7aab3d2b6d9e0d83ULL },
    { "iie/loresmixed/ntsc", 0xf0c84732083b795cULL },
    { "iie/loresmixed/medium", 0xf0c84732083b795cULL },
    { "iie/loresmixed/4bit", 0xf0c84732083b795cULL },
    { "iie/loresmixed/chatmauve", 0x32445ffef6b2e95cULL },
    { "iie/loresmixed/monowhite", 0xf0c84732083b795cULL },
    { "iie/loresmixed/monogreen", 0xf0c84732083b795cULL },
    { "iie/loresmixed/monoamber", 0xf0c84732083b795cULL },
    { "iie/loresmixed/signal", 0x82d923b6da184023ULL },
    { "iie/hgr/ntsc", 0xac4e9dd561ff842eULL },
    { "iie/hgr/medium", 0xde163ccdc0549453ULL },
    { "iie/hgr/4bit", 0xee7c2df576de4c85ULL },   // 4-bit phase fix (w & 0x0f)
    { "iie/hgr/chatmauve", 0xf9f97302dd16f583ULL },
    { "iie/hgr/monowhite", 0x75565f7ee4f7025cULL },
    { "iie/hgr/monogreen", 0xb953cafc4196e2dcULL },
    { "iie/hgr/monoamber", 0x16f1c2117085678cULL },
    { "iie/hgr/signal", 0x22b8536ef74be443ULL },
    { "iie/hgrmixed/ntsc", 0x5c24ad5625d0f235ULL },
    { "iie/hgrmixed/medium", 0x29d382b821c3f647ULL },
    { "iie/hgrmixed/4bit", 0x84fd3a4be3e2cfa0ULL },
    { "iie/hgrmixed/chatmauve", 0xd3a05f023c816503ULL },
    { "iie/hgrmixed/monowhite", 0x6fff8607d7464a83ULL },
    { "iie/hgrmixed/monogreen", 0x9708b82fdbd6a383ULL },
    { "iie/hgrmixed/monoamber", 0xa0b07365162b7393ULL },
    { "iie/hgrmixed/signal", 0x56d42a6c8d5a1883ULL },
    { "iie/dhgr/ntsc", 0xe15f3fd7367fd274ULL },
    { "iie/dhgr/medium", 0x809d733fa27113acULL },
    { "iie/dhgr/4bit", 0xee36a6f6e8a0a242ULL },
    { "iie/dhgr/chatmauve", 0x9adb0fbcb42d8883ULL },
    { "iie/dhgr/monowhite", 0xaae75d8da1df975cULL },
    { "iie/dhgr/monogreen", 0x3b3375014660335cULL },
    { "iie/dhgr/monoamber", 0xda582fcff14d474cULL },
    { "iie/dhgr/signal", 0x7b1f97d3d0debbbcULL },
    { "iie/dhgrmixed/ntsc", 0xfb11396ccb23e2f8ULL },
    { "iie/dhgrmixed/medium", 0x8e215ae6b9e4ffa7ULL },
    { "iie/dhgrmixed/4bit", 0x7a58c44cb8e7068dULL },
    { "iie/dhgrmixed/chatmauve", 0x9317aa71822cf903ULL },
    { "iie/dhgrmixed/monowhite", 0x6ca429b254b1395cULL },
    { "iie/dhgrmixed/monogreen", 0x05a0b21eb511375cULL },
    { "iie/dhgrmixed/monoamber", 0x4a271d50037a474cULL },
    { "iie/dhgrmixed/signal", 0x6326572f3aa44adcULL },
    { "ii+/text40/ntsc", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/medium", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/4bit", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/chatmauve", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/monowhite", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/monogreen", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/monoamber", 0x64b9ef4cc731c75cULL },
    { "ii+/text40/signal", 0x3643a247208629e3ULL },
    { "ii+/lores/ntsc", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/medium", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/4bit", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/chatmauve", 0xa78a672e2f668d03ULL },
    { "ii+/lores/monowhite", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/monogreen", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/monoamber", 0x85abcadd1bb83d83ULL },
    { "ii+/lores/signal", 0x7aab3d2b6d9e0d83ULL },
    { "ii+/hgr/ntsc", 0xac4e9dd561ff842eULL },
    { "ii+/hgr/medium", 0xde163ccdc0549453ULL },
    { "ii+/hgr/4bit", 0xee7c2df576de4c85ULL },
    { "ii+/hgr/chatmauve", 0xf9f97302dd16f583ULL },
    { "ii+/hgr/monowhite", 0x75565f7ee4f7025cULL },
    { "ii+/hgr/monogreen", 0xb953cafc4196e2dcULL },
    { "ii+/hgr/monoamber", 0x16f1c2117085678cULL },
    { "ii+/hgr/signal", 0x22b8536ef74be443ULL },
};

} // namespace

int main()
{
    const bool record = std::getenv("POM2_GOLDEN_RECORD") != nullptr;
    std::vector<std::pair<std::string, uint64_t>> results;

    for (const auto& sc : kScenes) {
        // pixels() per integer colour mode + the composite signal once.
        for (const auto& mo : kModes) {
            Memory mem;
            mem.setIIEMode(sc.iie);
            sc.setup(mem);

            Apple2Display disp;
            if (sc.iie) disp.setAuxMemory(mem.auxData());
            LeChatMauveCard chat;          // fresh; default COL140, colortext on
            disp.setChatMauveCard(&chat);
            disp.setHiResMode(mo.m);
            disp.render(mem);

            const size_t bytes =
                static_cast<size_t>(disp.width()) * disp.height() * sizeof(uint32_t);
            const uint64_t h = fnv1a(disp.pixels(), bytes);
            results.emplace_back(std::string(sc.name) + "/" + mo.tag, h);
        }

        // Composite signal generator (fillCompositeSignal) — integer 1-bit
        // waveform, hashed under the OE pipeline which always generates it.
        {
            Memory mem;
            mem.setIIEMode(sc.iie);
            sc.setup(mem);
            Apple2Display disp;
            if (sc.iie) disp.setAuxMemory(mem.auxData());
            LeChatMauveCard chat;
            disp.setChatMauveCard(&chat);
            disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
            disp.render(mem);
            assert(disp.signalProduced() && "OE signal must be produced for every scene");
            const size_t sbytes =
                static_cast<size_t>(disp.signalWidth()) * disp.signalHeight();
            const uint64_t h = fnv1a(disp.signal(), sbytes);
            results.emplace_back(std::string(sc.name) + "/signal", h);
        }
    }

    if (record) {
        for (const auto& r : results)
            std::printf("    { \"%s\", 0x%016llxULL },\n",
                        r.first.c_str(),
                        static_cast<unsigned long long>(r.second));
        std::printf("// %zu golden entries\n", results.size());
        return 0;
    }

    int failures = 0;
    for (const auto& r : results) {
        auto it = kGolden.find(r.first);
        if (it == kGolden.end()) {
            std::fprintf(stderr, "MISSING golden for %s (got 0x%016llx)\n",
                         r.first.c_str(),
                         static_cast<unsigned long long>(r.second));
            ++failures;
        } else if (it->second != r.second) {
            std::fprintf(stderr,
                         "MISMATCH %s: expected 0x%016llx got 0x%016llx\n",
                         r.first.c_str(),
                         static_cast<unsigned long long>(it->second),
                         static_cast<unsigned long long>(r.second));
            ++failures;
        }
    }
    if (failures) {
        std::fprintf(stderr, "display golden: %d mismatch(es)\n", failures);
        return 1;
    }
    std::printf("display golden hash: OK (%zu paths pinned)\n", results.size());
    return 0;
}
