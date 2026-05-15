// AudioDevice + dual-instance FloppySoundDevice smoke test.
//
// Pins two regressions from the 2026-05-15/16 mixer + 3.5" sound work:
//
//   1. Two FloppySoundDevice instances coexist with different form
//      factors — the 5.25" bank doesn't get clobbered when the 3.5"
//      instance loads its WAVs (each device stores a single sample
//      bank, so we needed two devices).
//
//   2. AudioDevice master volume + master mute apply post-mix:
//      master_mute=true  → output silent regardless of source content
//      master_vol=0.5    → output halved
//      master_vol=1.0    → output unchanged

#include "AudioDevice.h"
#include "FloppySoundDevice.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string findSampleDir()
{
    static const char* candidates[] = {
        "roms/floppy_samples",
        "../roms/floppy_samples",
        "../../roms/floppy_samples",
        "../../../roms/floppy_samples",
    };
    for (const char* p : candidates) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

// Constant-output source for the master gain test. Emits +0.5 on every
// sample so the energy is predictable and we can compare scaling
// directly.
class ConstSource : public AudioSource
{
public:
    explicit ConstSource(float v) : v_(v) {}
    void fillAudioBuffer(float* output, int frameCount) override
    {
        for (int i = 0; i < frameCount; ++i) output[i] = v_;
    }
private:
    float v_;
};

void testDualInstanceCoexistence(const std::string& dir)
{
    FloppySoundDevice fs525;
    FloppySoundDevice fs35;

    // Load each instance with its own form factor. The bug we're
    // pinning: a single shared FloppySoundDevice would have its sample
    // bank overwritten by the second loadSamples() call, leaving one
    // form factor silent. With two instances both must report loaded.
    assert(fs525.loadSamples(dir, FloppySoundDevice::FormFactor::FF525));
    assert(fs35 .loadSamples(dir, FloppySoundDevice::FormFactor::FF35));
    assert(fs525.isLoaded());
    assert(fs35 .isLoaded());
    assert(fs525.formFactor() == FloppySoundDevice::FormFactor::FF525);
    assert(fs35 .formFactor() == FloppySoundDevice::FormFactor::FF35);

    // Independent audible motor — the 525 bank still produces output
    // even after the 35 bank has been loaded "next to" it.
    fs525.setSampleRate(44100);
    fs525.setVolume(1.0f);
    fs525.motor(true, true);
    std::vector<float> buf(2048, 0.0f);
    fs525.fillAudioBuffer(buf.data(), 2048);
    double e525 = 0.0;
    for (float v : buf) e525 += static_cast<double>(v) * v;
    assert(e525 > 0.01);

    // Same for 35.
    fs35.setSampleRate(44100);
    fs35.setVolume(1.0f);
    fs35.motor(true, true);
    buf.assign(2048, 0.0f);
    fs35.fillAudioBuffer(buf.data(), 2048);
    double e35 = 0.0;
    for (float v : buf) e35 += static_cast<double>(v) * v;
    assert(e35 > 0.01);

    std::puts("OK dual_instance_coexistence");
}

void testMasterVolumeAndMute()
{
    AudioDevice dev;
    // mixSources is called by miniaudio's callback on a real device,
    // but we drive it directly here — independent of whether the
    // hardware audio init succeeded (headless CI commonly fails it).
    ConstSource src(0.5f);
    dev.addSource(&src);

    constexpr int kFrames = 256;
    std::vector<float> out(kFrames, 0.0f);

    // Default master = 1.0, unmuted: output should match source value
    // (clamped to ±1).
    dev.setMasterVolume(1.0f);
    dev.setMasterMuted(false);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[i] - 0.5f) < 1e-6f);
    }

    // Half volume.
    dev.setMasterVolume(0.5f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[i] - 0.25f) < 1e-6f);
    }

    // Master mute zeroes everything, regardless of master volume.
    dev.setMasterVolume(2.0f);
    dev.setMasterMuted(true);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(out[i] == 0.0f);
    }

    // Unmute + sane volume restores normal behaviour.
    dev.setMasterMuted(false);
    dev.setMasterVolume(1.0f);
    dev.mixSources(out.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        assert(std::fabs(out[i] - 0.5f) < 1e-6f);
    }

    dev.removeSource(&src);
    std::puts("OK master_volume_and_mute");
}

void testRateAwareAutoConfig()
{
    // FloppySoundDevice inherits RateAware. AudioDevice::addSource
    // should auto-call setSampleRate(actualSampleRate) so a future hot-
    // plug path that forgets the explicit call still gets configured.
    AudioDevice dev;
    FloppySoundDevice fs;
    dev.addSource(&fs);
    // We can't directly observe `outputSampleRate_` from outside, but
    // the contract is: after addSource the source is rate-configured
    // and ready to mix. A subsequent fillAudioBuffer must not crash on
    // an uninitialised sample rate (the previous buggy state had
    // outputSampleRate_=44100 by default — fine — but with the auto
    // path the value should track the negotiated device rate).
    std::vector<float> buf(64, 0.0f);
    fs.fillAudioBuffer(buf.data(), 64);   // must not crash, must not write
    for (float v : buf) assert(v == 0.0f);
    dev.removeSource(&fs);
    std::puts("OK rate_aware_auto_config");
}

}  // namespace

int main()
{
    std::puts("=== AudioDevice + dual FloppySoundDevice smoke test ===");
    const std::string dir = findSampleDir();
    if (dir.empty()) {
        std::puts("SKIP: roms/floppy_samples not found from cwd");
        return 0;
    }
    std::printf("Using sample dir: %s\n", dir.c_str());
    testDualInstanceCoexistence(dir);
    testMasterVolumeAndMute();
    testRateAwareAutoConfig();
    std::puts("All audio mixer smoke tests passed.");
    return 0;
}
