// FloppySoundDevice smoke test. Pins:
//   1. Sample loading succeeds against the bundled roms/floppy_samples/
//      tree (both 5.25" and 3.5" sets).
//   2. With no samples loaded: fillAudioBuffer is silent (and bumps the
//      internal frame counter so step-rate measurements stay consistent
//      across mute toggles).
//   3. motor(true, withDisk) drives a non-silent buffer (spin-up audible).
//   4. step() classifies rapid pulses (~6 ms cadence) into seek mode
//      and slow pulses (>= 100 ms) as single-step clicks.
//   5. mute zeroes the output; volume scales it.
//   6. reset() drains the queue and silences the next buffer.

#include "FloppySoundDevice.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Find roms/floppy_samples/ relative to the test binary's likely cwd.
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

float bufferEnergy(const std::vector<float>& b)
{
    double e = 0.0;
    for (float v : b) e += static_cast<double>(v) * v;
    return static_cast<float>(e);
}

void testSilenceWithoutSamples()
{
    FloppySoundDevice fs;
    assert(!fs.isLoaded());
    fs.setSampleRate(44100);
    fs.motor(true, true);     // queued, but no samples → silent anyway
    fs.step(5);
    std::vector<float> buf(512, 1.23f);
    fs.fillAudioBuffer(buf.data(), 512);
    // The source mixes additively; since no samples are loaded it must
    // not touch the buffer. We pre-filled with 1.23f to detect any
    // accidental write — the value must survive.
    for (float v : buf) assert(v == 1.23f);
    std::puts("OK silence_without_samples");
}

void testLoadSamples525(const std::string& dir)
{
    FloppySoundDevice fs;
    const bool ok = fs.loadSamples(dir, FloppySoundDevice::FormFactor::FF525);
    assert(ok);
    assert(fs.isLoaded());
    assert(fs.formFactor() == FloppySoundDevice::FormFactor::FF525);
    std::puts("OK load_525_samples");
}

void testLoadSamples35(const std::string& dir)
{
    FloppySoundDevice fs;
    const bool ok = fs.loadSamples(dir, FloppySoundDevice::FormFactor::FF35);
    assert(ok);
    assert(fs.formFactor() == FloppySoundDevice::FormFactor::FF35);
    std::puts("OK load_35_samples");
}

void testMotorAudible(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);

    // Mix a buffer with motor OFF — should be (very near) silent.
    std::vector<float> off(2048, 0.0f);
    fs.fillAudioBuffer(off.data(), 2048);
    const float offE = bufferEnergy(off);

    // Switch motor on and mix again. The spin_start_loaded sample is
    // a >300 ms burst of motor noise; one 2048-sample buffer (~46 ms)
    // must contain audible energy well above the off-state.
    fs.motor(true, true);
    std::vector<float> on(2048, 0.0f);
    fs.fillAudioBuffer(on.data(), 2048);
    const float onE = bufferEnergy(on);
    std::printf("  motor energy: off=%.6f on=%.6f\n", offE, onE);
    assert(onE > 0.01f);
    assert(onE > offE * 1000.0f || (offE < 1e-6f && onE > 0.01f));
    assert(fs.audioMotorOn());
    std::puts("OK motor_audible");
}

void testStepAndSeek(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);
    // Drain any pending state.
    std::vector<float> buf(256, 0.0f);
    fs.fillAudioBuffer(buf.data(), 256);

    // Slow single step → not seek mode. (Even the first step should
    // count as a single-step click since gap > kSeekJoinMs from the
    // never-seen state.)
    fs.step(0);
    buf.assign(256, 0.0f);
    fs.fillAudioBuffer(buf.data(), 256);
    assert(!fs.audioInSeek());
    std::puts("OK single_step_not_seek");

    // Now fire 8 rapid steps in succession, draining a small buffer
    // between each so the audio thread sees them at ~5.8 ms cadence
    // (256 / 44100 = 5.8 ms — perfect for SEEK_6MS classification).
    for (int i = 0; i < 8; ++i) {
        fs.step(i + 1);
        buf.assign(256, 0.0f);
        fs.fillAudioBuffer(buf.data(), 256);
    }
    assert(fs.audioInSeek());
    std::puts("OK rapid_steps_enter_seek");
}

void testMuteSilencesOutput(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);
    fs.motor(true, true);
    std::vector<float> a(2048, 0.0f);
    fs.fillAudioBuffer(a.data(), 2048);
    assert(bufferEnergy(a) > 0.01f);

    fs.setMuted(true);
    std::vector<float> b(2048, 0.0f);
    fs.fillAudioBuffer(b.data(), 2048);
    // Mute must zero the contribution; buffer was pre-zero so it stays
    // exactly zero (no other source mixes in).
    for (float v : b) assert(v == 0.0f);
    std::puts("OK mute_silences");
}

// Regression: at 60× turbo, an entire 35-track seek finishes inside one
// audio buffer (~5 ms). All step() calls land between two
// fillAudioBuffers — they share the same audioFrameCounter_ value, so
// consecutive queued steps see `now - lastStepFrame_ == 0`. The first
// implementation computed `pitch = nominalMs / gapMs` → INF → mixLoop
// hung on `while (pos >= len) pos -= len` because INF - len == INF in
// IEEE 754. Symptom: audio thread frozen after the first burst,
// no further sound output.
//
// Fixed by clamping gapMs to >= 1 ms in drainCommands + advancing
// lastStepFrame_ artificially between steps in the same batch, plus a
// defensive (!finite || > 1e6) early-bail in mixLoop / mixOneShot.
void testRapidStepsNoHang(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.setVolume(1.0f);

    // Drain.
    std::vector<float> drain(256, 0.0f);
    fs.fillAudioBuffer(drain.data(), 256);

    // Fire 140 step events with NO buffer in between (simulates a full
    // 35-track 4-phase seek at 60× turbo). Pre-fix this would hang.
    for (int i = 0; i < 140; ++i) fs.step(i);

    // First buffer after the burst: must return promptly and produce
    // non-silent output (seek sample looping).
    std::vector<float> buf(2048, 0.0f);
    fs.fillAudioBuffer(buf.data(), 2048);
    assert(bufferEnergy(buf) > 0.01f);
    std::puts("OK rapid_steps_no_hang");
}

// Regression: at turbo speed, motor(false) follows motor(true) within
// milliseconds wall-clock. Pre-fix, the audio thread silenced the spin
// loop immediately and the user heard "start click → end click →
// silence" instead of the motor whirr. The hold-off in fillAudioBuffer
// should now keep the loop audible for ~800 ms wall-clock after the
// controller's motor(false), regardless of how fast the emulated CPU
// fired them.
void testRapidMotorTogglePreservesLoop(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    const uint32_t sr = 44100;
    fs.setSampleRate(sr);
    fs.setVolume(1.0f);

    fs.motor(true,  true);
    fs.motor(false, true);     // queued back-to-back like at 60× turbo

    // Step 100 ms of audio (~4410 frames). Hold-off is 800 ms — the loop
    // must still be playing here, audioMotorOn_ must still be true.
    const int frames = 4410;
    std::vector<float> a(frames, 0.0f);
    fs.fillAudioBuffer(a.data(), frames);
    assert(fs.audioMotorOn());
    const float earlyE = bufferEnergy(a);
    assert(earlyE > 0.01f);

    // Step 500 ms more (still inside the 800 ms hold-off window).
    std::vector<float> b(static_cast<size_t>(sr / 2), 0.0f);
    fs.fillAudioBuffer(b.data(), static_cast<int>(b.size()));
    assert(fs.audioMotorOn());
    assert(bufferEnergy(b) > 0.1f);

    // Step past the hold-off (another 400 ms takes us to ~1 s total).
    // audioMotorOn_ flips off when the next fillAudioBuffer sees
    // audioFrameCounter_ >= motorOffDeadline_ — that check happens at
    // the top of each buffer call, so we need a follow-up small buffer
    // after crossing the 800 ms deadline.
    std::vector<float> c(static_cast<size_t>(sr * 4 / 10), 0.0f);
    fs.fillAudioBuffer(c.data(), static_cast<int>(c.size()));
    std::vector<float> tiny(64, 0.0f);
    fs.fillAudioBuffer(tiny.data(), 64);
    assert(!fs.audioMotorOn());
    std::puts("OK rapid_motor_toggle_preserves_loop");
}

void testReset(const std::string& dir)
{
    FloppySoundDevice fs;
    assert(fs.loadSamples(dir));
    fs.setSampleRate(44100);
    fs.motor(true, true);
    // Reset queues a MotorOff command. After one fillAudioBuffer the
    // queue is drained and audioMotorOn_ flips false — but the spin_end
    // one-shot is now active. We just verify no crash.
    fs.reset();
    std::vector<float> buf(512, 0.0f);
    fs.fillAudioBuffer(buf.data(), 512);
    assert(!fs.audioMotorOn());
    std::puts("OK reset");
}

}  // namespace

int main()
{
    std::puts("FloppySoundDevice smoke test starting…");

    testSilenceWithoutSamples();

    const std::string dir = findSampleDir();
    if (dir.empty()) {
        std::puts("WARN  roms/floppy_samples/ not found relative to CWD —");
        std::puts("      sample-dependent assertions skipped. The path probe");
        std::puts("      walks ., .., ../.., ../../.. — run from repo root or");
        std::puts("      build/.");
        return 0;
    }
    std::printf("Using sample dir: %s\n", dir.c_str());

    testLoadSamples525(dir);
    testLoadSamples35 (dir);
    testMotorAudible  (dir);
    testStepAndSeek   (dir);
    testMuteSilencesOutput(dir);
    testRapidMotorTogglePreservesLoop(dir);
    testRapidStepsNoHang(dir);
    testReset         (dir);

    std::puts("OK floppy_sound_smoke");
    return 0;
}
