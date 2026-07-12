// POM2 Apple II Emulator
// Copyright (C) 2026 Verhille Arnaud
//
// DHGR NTSC 8-px chroma palette (][-pix, BSD-2-Clause) - see the .cpp for
// provenance. Indexed [x%4][trailing 8-dot pattern][rgb]; pattern bit 7 is
// the dot itself, bit 0 the dot 7 positions earlier.

#ifndef HGRPAINT_DHGR_NTSC8_PALETTE_H
#define HGRPAINT_DHGR_NTSC8_PALETTE_H

#include <cstdint>

namespace hgrpaint {
extern const uint8_t kDhgrNtsc8Srgb[4][256][3];
} // namespace hgrpaint

#endif // HGRPAINT_DHGR_NTSC8_PALETTE_H
