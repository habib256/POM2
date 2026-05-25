// CassetteDevice WAV tail-flush regression test.
//
// pcmToDurations emits a transition-duration only ON a sign change, so the
// held segment after the LAST zero-crossing (trailing leader / final
// half-bit — saveWavTape appends a 0.1 s trailer) was never converted,
// truncating the recovered transition stream by one segment. This builds a
// tiny WAV whose level pattern is +,−,+ and ends held on +, so the correct
// transition count is 3 (the two mid sign-changes PLUS the final tail);
// the bug produced 2.

#include "CassetteDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
namespace fs = std::filesystem;

void putU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
}
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

// Minimal 16-bit PCM mono WAV from int16 samples.
std::string writeWav(const std::vector<int16_t>& samples) {
    const uint32_t rate = 44100;
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> w;
    w.insert(w.end(), {'R','I','F','F'}); putU32(w, 36 + dataSize);
    w.insert(w.end(), {'W','A','V','E'});
    w.insert(w.end(), {'f','m','t',' '}); putU32(w, 16);
    putU16(w, 1);            // PCM
    putU16(w, 1);            // mono
    putU32(w, rate);
    putU32(w, rate * 2);     // byte rate
    putU16(w, 2);            // block align
    putU16(w, 16);           // bits/sample
    w.insert(w.end(), {'d','a','t','a'}); putU32(w, dataSize);
    for (int16_t s : samples) putU16(w, static_cast<uint16_t>(s));

    const auto p = fs::temp_directory_path() / "pom2_cassette_tail.wav";
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WAV");
    f.write(reinterpret_cast<const char*>(w.data()),
            static_cast<std::streamsize>(w.size()));
    return p.string();
}

}  // namespace

int main()
{
    // Level pattern +,−,+ (4 samples each), ending held on +.
    constexpr int16_t HI = 16000, LO = -16000;
    std::vector<int16_t> samples;
    for (int i = 0; i < 4; ++i) samples.push_back(HI);   // initial level +
    for (int i = 0; i < 4; ++i) samples.push_back(LO);   // transition 1: +→−
    for (int i = 0; i < 4; ++i) samples.push_back(HI);   // transition 2: −→+, then held tail

    const std::string path = writeWav(samples);

    CassetteDevice cass;
    if (!cass.loadTape(path)) {
        std::printf("FAIL: loadTape(%s)\n", path.c_str());
        return 1;
    }
    const size_t n = cass.getLoadedTransitionCount();
    // 2 mid sign-changes + 1 flushed tail segment = 3. (Bug = 2.)
    if (n != 3) {
        std::printf("FAIL: transition count = %zu (expected 3 — tail not flushed?)\n", n);
        return 1;
    }
    std::printf("OK cassette_wav_tail (3 transitions incl. flushed tail)\n");
    return 0;
}
