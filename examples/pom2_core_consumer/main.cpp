#include <pom2/core.hpp>

#include <array>
#include <cstdio>

int main()
{
    pom2::Core core;
    core.setAudioSampleRate(44100);
    core.setJoystickAxes(0.0f, 0.0f);
    core.write(0x0300, 0x42);
    core.hardReset();
    core.run(32);

    const pom2::CpuState cpu = core.cpuState();
    const pom2::FramebufferView frame = core.renderFrame();
    std::array<float, 512> stereo{};
    const std::size_t audioFrames = core.pullAudio(
        stereo.data(), stereo.size() / 2);
    std::printf("PC=%04X cycles=%llu RAM[0300]=%02X frame=%dx%d audio=%zu\n",
                static_cast<unsigned>(cpu.programCounter),
                static_cast<unsigned long long>(cpu.cycles),
                static_cast<unsigned>(core.read(0x0300)),
                frame.width, frame.height, audioFrames);
    return 0;
}
