// DIGIDREAM 1 (French Touch) Mockingboard audio A/B probe — diagnostic,
// NOT a pinned test (needs disks_5.4/demo/digidream/DD.dsk + roms/).
//
// Why
// ---
// DD1 drives BOTH AY chips, adds a ~6.8 kHz 4-bit PCM "digidrum" written
// into a volume register from a VIA1 T1 IRQ, and runs a hardware-envelope
// buzzer on channel A. That is the densest register traffic in the corpus,
// so it is the material that exposes any regression in the audio-thread
// replay path. This probe renders DD1's real boot to a .wav so two builds
// of `Mockingboard.cpp` (worktree vs `git show HEAD:`) can be compared
// sample-for-sample.
//
// What it does
// ------------
// Boots DD.dsk on //e Enhanced PAL (128 K) with a Disk II in slot 6 and a
// MockingboardCard in slot 4, then drives the machine in the SAME cadence
// the real emulator uses:
//
//   per wall-clock 50 Hz frame:
//     * run one video frame of CPU cycles (20313 PAL — or 1'000'000 when
//       the Disk II motor is on, which is exactly what MainWindow's
//       `disk_turbo` does, MainWindow.cpp:6789-6797);
//     * pull 882 audio samples (44100/50) from the card's AudioSource in
//       256-frame buffers, carrying the fractional remainder — the same
//       granularity mismatch the real AudioDevice callback has.
//
// While stepping it decodes the AY control bus through `peekViaRegister`
// exactly as `dd2_ay_trace` does, so the register write log carries a CPU
// cycle stamp and can be correlated against the rendered audio.
//
// Outputs (prefix from argv[3], default ./dd1):
//   <prefix>.wav   16-bit mono 44.1 kHz render
//   <prefix>.csv   AY write log: cycle,chip,reg,val
//   <prefix>.frames.csv  per-frame: frame,cycle,motor,ayWrites,samples
//
// Env POM2_MB_TRACE=<path> is honoured by the *instrumented* copy of
// Mockingboard.cpp only (see tests/CMakeLists.txt dd1_audio_instr).
//
// Usage: build/tests/dd1_audio_ab [wall-seconds] [disk] [out-prefix] [turbo0|1]

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "CpuClock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string findFirst(std::initializer_list<const char*> cands)
{
    std::error_code ec;
    for (const char* c : cands)
        if (std::filesystem::is_regular_file(c, ec)) return c;
    return {};
}

struct AyWrite { uint64_t cycle; uint8_t chip; uint8_t reg; uint8_t val; };

void writeWav(const std::string& path, const std::vector<float>& s, uint32_t sr)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
    const uint32_t n = static_cast<uint32_t>(s.size());
    const uint32_t dataBytes = n * 2u;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32(sr); u32(sr * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    std::vector<int16_t> pcm(n);
    for (uint32_t i = 0; i < n; ++i) {
        float v = s[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }
    std::fwrite(pcm.data(), 2, n, f);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv)
{
    const double wallSecs = (argc > 1) ? std::atof(argv[1]) : 45.0;
    const std::string rom  = findFirst({"../roms/apple2e.rom", "roms/apple2e.rom"});
    const std::string boot = findFirst({"../roms/disk2.rom",   "roms/disk2.rom"});
    const std::string p6   = findFirst({"../roms/diskii_p6.rom", "roms/diskii_p6.rom"});
    const std::string dsk  = (argc > 2 && argv[2][0]) ? std::string(argv[2]) : findFirst({
        "../disks_5.4/demo/digidream/DD.dsk",
        "disks_5.4/demo/digidream/DD.dsk"});
    const std::string prefix = (argc > 3) ? std::string(argv[3]) : std::string("dd1");
    const bool turbo = (argc > 4) ? (std::atoi(argv[4]) != 0) : true;
    if (rom.empty() || boot.empty() || dsk.empty()) {
        std::printf("dd1_audio_ab SKIP: missing apple2e.rom / disk2.rom / DD.dsk\n");
        return 0;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    mem.setVideoStandard(VideoStandard::PAL);
    if (!mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/false)) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    auto diskOwned = std::make_unique<DiskIICard>();
    DiskIICard* disk = diskOwned.get();
    if (!disk->loadBootRom(boot) || !disk->insertDisk(dsk)) {
        std::fprintf(stderr, "Disk II setup failed\n"); return 1;
    }
    if (!p6.empty()) disk->loadLssRom(p6);
    mem.slotBus().plug(6, std::move(diskOwned));

    auto mbOwned = std::make_unique<MockingboardCard>(4);
    MockingboardCard* mb = mbOwned.get();
    mem.slotBus().plug(4, std::move(mbOwned));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    // MUST mirror MainWindow::plugMockingboard (MainWindow.cpp:1530): without
    // it `cpu_` stays null, `lastSyncCycle_` never leaves 0, EVERY AY event is
    // stamped cycle 0 and `latestAyEventCycle_` never rises — which silently
    // disables the whole cycle-stamped replay path being measured here.
    mb->setCpu(&cpu);
    cpu.hardReset();
    mem.slotBus().reset();

    const auto vt = pom2VideoTiming(VideoStandard::PAL);
    const double cpuHz = static_cast<double>(vt.cpuClockHz);
    constexpr uint32_t kSampleRate = 44100;
    constexpr int      kBufFrames  = 256;

    // Same wiring MainWindow does: the card learns the live CPU clock and
    // the device sample rate before any audio is pulled.
    mb->setSampleRate(kSampleRate);
    mb->setCpuClock(cpuHz);
    mb->setVolume(1.0f);            // unity so the render is the raw mix
    AudioSource* src = mb->audioSource();

    const int    totalFrames    = static_cast<int>(wallSecs * vt.refreshHz);
    const double samplesPerFrame =
        static_cast<double>(kSampleRate) / static_cast<double>(vt.refreshHz);

    std::vector<AyWrite> log;
    log.reserve(4u << 20);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(wallSecs * kSampleRate) + 4096);
    std::vector<float> buf(kBufFrames);

    uint8_t prevPb[2] = {0xFF, 0xFF};
    uint8_t latch[2]  = {0, 0};

    FILE* fr = std::fopen((prefix + ".frames.csv").c_str(), "w");
    if (fr) std::fprintf(fr, "frame,cycle,motor,ayWritesCum,samplesCum\n");

    double samplesOwed = 0.0;
    uint64_t turboFrames = 0;
    for (int fnum = 0; fnum < totalFrames; ++fnum) {
        const bool motor = disk->isMotorOn();
        const int64_t budget = (turbo && motor) ? 1'000'000 : vt.cyclesPerFrame;
        if (turbo && motor) ++turboFrames;
        const uint64_t target = cpu.getCycleCountNow() + static_cast<uint64_t>(budget);
        while (cpu.getCycleCountNow() < target) {
            cpu.step();
            for (int ci = 0; ci < 2; ++ci) {
                const uint8_t pb = static_cast<uint8_t>(mb->peekViaRegister(ci, 0) & 0x07);
                if (pb == prevPb[ci]) continue;
                prevPb[ci] = pb;
                if (pb == 0x07) {
                    latch[ci] = static_cast<uint8_t>(mb->peekViaRegister(ci, 1) & 0x0F);
                } else if (pb == 0x06) {
                    log.push_back({cpu.getCycleCountNow(), static_cast<uint8_t>(ci),
                                   latch[ci], mb->peekViaRegister(ci, 1)});
                }
            }
        }
        samplesOwed += samplesPerFrame;
        while (samplesOwed >= kBufFrames) {
            src->fillAudioBuffer(buf.data(), kBufFrames);
            out.insert(out.end(), buf.begin(), buf.end());
            samplesOwed -= kBufFrames;
        }
        if (fr) std::fprintf(fr, "%d,%llu,%d,%zu,%zu\n", fnum,
                             (unsigned long long)cpu.getCycleCountNow(),
                             motor ? 1 : 0, log.size(), out.size());
    }
    if (fr) std::fclose(fr);

    writeWav(prefix + ".wav", out, kSampleRate);
    if (FILE* f = std::fopen((prefix + ".csv").c_str(), "w")) {
        for (const auto& w : log)
            std::fprintf(f, "%llu,%u,%u,%u\n", (unsigned long long)w.cycle,
                         w.chip, w.reg, w.val);
        std::fclose(f);
    }

    std::printf("DD1 A/B probe: %.1f wall s (%d frames, %llu in disk turbo), "
                "%llu emulated CPU cycles (%.1f emulated s), PC=$%04X\n",
                wallSecs, totalFrames, (unsigned long long)turboFrames,
                (unsigned long long)cpu.getCycleCountNow(),
                cpu.getCycleCountNow() / cpuHz, cpu.getProgramCounter());
    std::printf("AY writes: %zu (chip0=%u chip1=%u), samples rendered: %zu (%.2f s)\n",
                log.size(), mb->getAyWriteCount(0), mb->getAyWriteCount(1),
                out.size(), out.size() / double(kSampleRate));

    // ── register histogram per chip ──────────────────────────────────────
    uint32_t hist[2][16] = {};
    for (const auto& w : log) hist[w.chip][w.reg]++;
    std::printf("\nregister write histogram\n  reg   AY1($C400)   AY2($C480)\n");
    for (int r = 0; r < 16; ++r)
        if (hist[0][r] || hist[1][r])
            std::printf("  R%-2d %10u %12u\n", r, hist[0][r], hist[1][r]);

    // ── R13 (envelope shape) histogram — hypothesis (e) ─────────────────
    std::map<int, uint32_t> shape[2];
    for (const auto& w : log) if (w.reg == 13) shape[w.chip][w.val & 0x0F]++;
    for (int c = 0; c < 2; ++c) {
        if (shape[c].empty()) continue;
        std::printf("\nR13 shapes on AY%d: ", c + 1);
        for (auto& [s, n] : shape[c]) {
            const bool cont = (s & 0x08) != 0;
            const bool hold = cont ? ((s & 0x01) != 0) : true;
            std::printf("$%X(%u,%s) ", s, n, hold ? "HOLD" : "cont");
        }
        std::putchar('\n');
    }

    // ── digidrum: amplitude-register writes closer than one replay tick ──
    for (int c = 0; c < 2; ++c) {
        for (int rg = 8; rg <= 10; ++rg) {
            std::vector<uint64_t> dg;
            uint64_t p = 0;
            for (const auto& w : log)
                if (w.chip == c && w.reg == rg) {
                    if (p && w.cycle - p < 1000) dg.push_back(w.cycle - p);
                    p = w.cycle;
                }
            if (dg.size() > 50) {
                std::sort(dg.begin(), dg.end());
                std::printf("digidrum AY%d R%d: n=%zu median gap %llu cycles"
                            " -> %.0f Hz\n", c + 1, rg, dg.size(),
                            (unsigned long long)dg[dg.size() / 2],
                            cpuHz / double(dg[dg.size() / 2]));
            }
        }
    }

    // ── short-time RMS envelope (10 ms hops), printed sparsely ──────────
    {
        const size_t hop = kSampleRate / 100;
        std::printf("\nshort-time RMS (100 Hz hop) — first non-silent hop and "
                    "per-second mean/peak\n");
        size_t firstNz = 0;
        for (size_t i = 0; i + hop <= out.size(); i += hop) {
            double e = 0; for (size_t k = 0; k < hop; ++k) e += out[i+k]*out[i+k];
            if (std::sqrt(e / hop) > 1e-4) { firstNz = i; break; }
        }
        std::printf("  first audio at %.2f s\n", firstNz / double(kSampleRate));
        for (size_t s0 = 0; s0 + kSampleRate <= out.size(); s0 += kSampleRate) {
            double mean = 0, peak = 0; int nh = 0;
            for (size_t i = s0; i + hop <= s0 + kSampleRate; i += hop) {
                double e = 0; for (size_t k = 0; k < hop; ++k) e += out[i+k]*out[i+k];
                const double r = std::sqrt(e / hop);
                mean += r; peak = std::max(peak, r); ++nh;
            }
            std::printf("  t=%3.0fs rms_mean=%.5f rms_peak=%.5f\n",
                        s0 / double(kSampleRate), mean / nh, peak);
        }
    }
    return 0;
}
