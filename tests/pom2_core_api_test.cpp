// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <pom2/core.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void writeLe32(std::ofstream& output, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24),
    };
    output.write(bytes, sizeof(bytes));
}

void writeTestCassette(const std::string& path)
{
    std::ofstream tape(path, std::ios::binary);
    tape.write("POM1ACI1", 8);
    tape.put(1); // format version
    tape.put(0); // initial comparator level
    tape.put(0);
    tape.put(0);
    writeLe32(tape, 1);    // one transition
    writeLe32(tape, 1024); // after 1024 emulated CPU cycles
    assert(tape.good());
}

} // namespace

int main()
{
    pom2::Core core;

    const std::string sourceDir = POM2_TEST_SOURCE_DIR;
    assert(core.loadRom(sourceDir + "/roms/apple2.rom"));
    assert(core.loadCharacterRom(sourceDir + "/roms/apple2_char.rom"));
    assert(core.attachDiskII(sourceDir + "/roms/disk2.rom",
                             sourceDir + "/roms/diskii_p6.rom"));
    assert(!core.diskInserted());
    assert(!core.bootDisk());
    assert(!core.lastError().empty());

    const std::string diskPath = "pom2_core_api_blank.dsk";
    {
        std::ofstream disk(diskPath, std::ios::binary);
        const std::vector<char> blank(35u * 16u * 256u, 0);
        disk.write(blank.data(), static_cast<std::streamsize>(blank.size()));
        assert(disk.good());
    }
    assert(core.insertDisk(0, diskPath));
    assert(core.diskInserted());
    assert(core.bootDisk());
    assert(core.cpuState().programCounter == 0xC600);
    assert(core.ejectDisk(0));
    assert(!core.diskInserted());
    assert(std::remove(diskPath.c_str()) == 0);

    core.write(0x0300, 0x42);
    assert(core.read(0x0300) == 0x42);

    assert(core.pasteText("PRINT 1\n") == 8);
    core.queueKey('A');

    pom2::Core inputCore;
    assert(inputCore.setPaddleButton(0, true));
    assert((inputCore.read(0xC061) & 0x80) != 0);
    assert(inputCore.setPaddleButton(0, false));
    assert((inputCore.read(0xC061) & 0x80) == 0);
    assert(!inputCore.setPaddleButton(3, true));
    assert(!inputCore.lastError().empty());

    // Normalized axes map to the RC endpoints without pulling GLFW/SDL into
    // the core. A $C070 strobe starts both timers at the same CPU cycle.
    inputCore.setJoystickAxes(-1.0f, 1.0f);
    (void)inputCore.read(0xC070);
    assert((inputCore.read(0xC064) & 0x80) == 0);
    assert((inputCore.read(0xC065) & 0x80) != 0);

    // Raw paddle 0 value 20 expires after 20*11 = 220 emulated cycles.
    assert(inputCore.setPaddle(0, 20));
    assert(inputCore.lastError().empty());
    (void)inputCore.read(0xC070);
    assert((inputCore.read(0xC064) & 0x80) != 0);
    const int paddleEarlyCycles = inputCore.run(100);
    assert(paddleEarlyCycles >= 100 && paddleEarlyCycles < 220);
    assert((inputCore.read(0xC064) & 0x80) != 0);
    assert(inputCore.run(200) >= 200);
    assert((inputCore.read(0xC064) & 0x80) == 0);
    assert(!inputCore.setPaddle(4, 128));
    assert(!inputCore.lastError().empty());

    const pom2::FramebufferView frame = core.renderFrame();
    assert(frame.pixels != nullptr);
    assert(frame.width == 280);
    assert(frame.height == 192);

    std::vector<float> silence(256, 1.0f);
    assert(core.pullSpeakerAudio(silence.data(), silence.size(), 44100)
           == silence.size());
    assert(std::all_of(silence.begin(), silence.end(),
                       [](float sample) { return sample == 0.0f; }));
    assert(core.pullSpeakerAudio(nullptr, 16, 44100) == 0);
    assert(core.pullSpeakerAudio(silence.data(), silence.size(), 0) == 0);

    core.hardReset();
    const std::uint64_t before = core.cpuState().cycles;
    const int executed = core.run(16);
    const pom2::CpuState after = core.cpuState();
    assert(executed >= 16);
    assert(after.cycles > before);

    core.setSpeakerVolume(2.0f);
    for (int i = 0; i < 64; ++i) {
        core.run(16);
        (void)core.read(0xC030);
    }
    std::vector<float> speakerPcm(1024, 0.0f);
    assert(core.pullSpeakerAudio(speakerPcm.data(), speakerPcm.size(), 48000)
           == speakerPcm.size());
    assert(std::all_of(speakerPcm.begin(), speakerPcm.end(),
                       [](float sample) {
                           return std::isfinite(sample)
                               && sample >= -1.0f && sample <= 1.0f;
                       }));
    assert(std::any_of(speakerPcm.begin(), speakerPcm.end(),
                       [](float sample) { return std::abs(sample) > 1.0e-6f; }));

    const std::string cassettePath = "pom2_core_api_input.aci";
    const std::string recordingPath = "pom2_core_api_output.aci";
    writeTestCassette(cassettePath);

    pom2::Core cassetteCore;
    cassetteCore.setAudioSampleRate(44100);
    assert(cassetteCore.loadCassette(cassettePath));
    assert(cassetteCore.cassetteState().loaded);
    assert(!cassetteCore.cassetteState().playEngaged);

    // Loading makes a short mechanical click. Drain it so the signal checked
    // below can only come from the cycle-driven program tape.
    std::vector<float> cassetteStereo(4096 * 2, 0.0f);
    assert(cassetteCore.pullAudio(cassetteStereo.data(), 4096, 44100) == 4096);

    cassetteCore.playCassette();
    assert(cassetteCore.cassetteState().playEngaged);
    cassetteCore.pauseCassette(true);
    assert(cassetteCore.cassetteState().paused);
    cassetteCore.pauseCassette(false);
    assert(!cassetteCore.cassetteState().paused);

    // PLAY arms a program tape. The first $C060 read starts transport, then
    // Memory::advanceCycles moves the comparator at the recorded cycle stamp.
    assert((cassetteCore.read(0xC060) & 0x80) == 0x00);
    assert(cassetteCore.run(1200) >= 1200);
    assert((cassetteCore.read(0xC060) & 0x80) == 0x80);

    cassetteStereo.assign(512 * 2, 0.0f);
    assert(cassetteCore.pullAudio(cassetteStereo.data(), 512, 44100) == 512);
    bool cassetteAudible = false;
    for (std::size_t i = 0; i < 512; ++i) {
        const float left = cassetteStereo[2 * i];
        const float right = cassetteStereo[2 * i + 1];
        assert(std::isfinite(left) && std::isfinite(right));
        assert(std::abs(left - right) < 1.0e-7f);
        cassetteAudible = cassetteAudible || std::abs(left) > 1.0e-6f;
    }
    assert(cassetteAudible);

    cassetteCore.rewindCassette();
    assert(cassetteCore.cassetteState().rewinding);
    cassetteCore.run(128);
    assert(!cassetteCore.cassetteState().rewinding);
    cassetteCore.playCassette();
    cassetteCore.stopCassette();
    assert(!cassetteCore.cassetteState().playEngaged);

    // A computer reset preserves the physical tape, while $C020 output is
    // captured independently and can be saved by the embedding host.
    cassetteCore.hardReset();
    assert(cassetteCore.cassetteState().loaded);
    cassetteCore.armCassetteRecording();
    cassetteCore.run(256);
    cassetteCore.write(0xC020, 0);
    assert(cassetteCore.cassetteState().hasRecording);
    assert(cassetteCore.saveCassette(recordingPath));
    cassetteCore.clearCassetteRecording();
    assert(!cassetteCore.cassetteState().hasRecording);
    cassetteCore.ejectCassette();
    assert(!cassetteCore.cassetteState().loaded);
    assert(std::remove(cassettePath.c_str()) == 0);
    assert(std::remove(recordingPath.c_str()) == 0);

    pom2::Core audioCore;
    audioCore.hardReset();
    audioCore.run(32);
    assert(audioCore.attachMockingboard());
    assert(audioCore.mockingboardAttached());
    assert(!audioCore.attachMockingboard());
    assert(!audioCore.lastError().empty());

    const auto viaWrite = [&](std::uint16_t offset, std::uint8_t value) {
        audioCore.write(static_cast<std::uint16_t>(0xC400 + offset), value);
    };
    const auto ayWriteLeft = [&](std::uint8_t reg, std::uint8_t value) {
        viaWrite(0x03, 0xFF); // DDRA
        viaWrite(0x02, 0xFF); // DDRB
        viaWrite(0x01, reg);  // ORA
        viaWrite(0x00, 0x07); // latch
        viaWrite(0x00, 0x04); // inactive
        viaWrite(0x01, value);
        viaWrite(0x00, 0x06); // write
        viaWrite(0x00, 0x04); // inactive
    };
    ayWriteLeft(0, 0x00); // tone period $0100
    ayWriteLeft(1, 0x01);
    ayWriteLeft(7, 0x3E); // tone A only
    ayWriteLeft(8, 0x0F); // full channel-A amplitude
    audioCore.run(17045);

    constexpr std::size_t kStereoFrames = 4096;
    std::vector<float> stereo(kStereoFrames * 2, 0.0f);
    assert(audioCore.pullAudio(stereo.data(), kStereoFrames, 44100)
           == kStereoFrames);
    assert(audioCore.pullAudio(stereo.data(), kStereoFrames, 44100)
           == kStereoFrames); // second block: DC filter settled
    double leftPower = 0.0;
    double rightPower = 0.0;
    for (std::size_t i = 0; i < kStereoFrames; ++i) {
        assert(std::isfinite(stereo[2 * i]));
        assert(std::isfinite(stereo[2 * i + 1]));
        leftPower += static_cast<double>(stereo[2 * i]) * stereo[2 * i];
        rightPower += static_cast<double>(stereo[2 * i + 1])
                    * stereo[2 * i + 1];
    }
    const double leftRms = std::sqrt(leftPower / kStereoFrames);
    const double rightRms = std::sqrt(rightPower / kStereoFrames);
    assert(leftRms > 1.0e-3);
    assert(rightRms < leftRms * 0.05);
    assert(audioCore.pullAudio(nullptr, 16, 44100) == 0);

    pom2::Core moved(std::move(core));
    moved.write(0x0301, 0xA5);
    assert(moved.read(0x0301) == 0xA5);

    pom2::Core cmos({
        pom2::CpuModel::CMOS65C02,
        pom2::VideoTiming::PAL,
        true,
    });
    cmos.coldBoot();
    cmos.step();
    assert(cmos.cpuState().cycles > 0);

    assert(!cmos.loadRom("this-rom-does-not-exist.bin"));
    assert(!cmos.lastError().empty());
    return 0;
}
