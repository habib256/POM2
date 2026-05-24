// POM2 Apple II Emulator
//
// 6502 clock: 1.0227 MHz. The Apple II master oscillator runs at
// 14.31818 MHz; the CPU clock is that divided by 14, with a "long cycle"
// every 65 cycles to keep television scan-line timing aligned with NTSC
// (colour subcarrier). We use the nominal value — long-cycle timing
// isn't modelled.

#ifndef POM2_CPU_CLOCK_H
#define POM2_CPU_CLOCK_H

inline constexpr int POM2_CPU_CLOCK_HZ = 1022727;
inline constexpr int POM2_CPU_CYCLES_PER_FRAME_60HZ = (POM2_CPU_CLOCK_HZ + 30) / 60;
inline constexpr int POM2_CPU_CYCLES_PER_MILLISECOND = (POM2_CPU_CLOCK_HZ + 500) / 1000;

#endif // POM2_CPU_CLOCK_H
