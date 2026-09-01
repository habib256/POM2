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

// Apple2Display_Internal.h — helpers shared by Apple2Display.cpp and the
// renderer TUs split out of it (Apple2Display_ChatMauve.cpp): which video
// page the scanner shows under 80STORE, and the band ∩ rows/scanlines
// clipping every painter uses. Header-only, internal to the display family.

#ifndef POM2_APPLE2_DISPLAY_INTERNAL_H
#define POM2_APPLE2_DISPLAY_INTERNAL_H

#include "Memory.h"

#include <algorithm>

namespace pom2::a2disp {

// Sather "Understanding the Apple IIe" §5-25 table 5.10: PAGE2 only steers
// the video scanner to page 2 when 80STORE is off (text/lo-res) or when
// 80STORE+HIRES are not both on (HGR). Otherwise PAGE2 is repurposed as a
// MAIN/AUX memory bank switch — the scanner stays locked to page 1. Sierra
// AGI/SCI titles in DHGR (Space Quest II, King's Quest, …) toggle PAGE2
// every byte to interleave aux+main nibbles into HGR page 1; treating that
// as a video-page flip displays the uninitialised $4000 area as garbage.
inline bool videoTextPage2(const Memory::DisplayState& s)
{
    return s.page2 && !s.eightyStore;
}

inline bool videoHgrPage2(const Memory::DisplayState& s)
{
    return s.page2 && !(s.eightyStore && s.hiRes);
}

// Text rows [rowLo, rowHi) whose 8-scanline cells INTERSECT the band
// [scanY0, scanY1) — rounded OUTWARD, so a row straddling a beam-split is
// returned for both bands. Each band's painter then clips its writes to its
// own scanline window (the clipY0/clipY1 painter args), so the straddled
// row is painted twice but each scanline exactly once, with the display
// state active for ITS band. (The old inward rounding returned straddled
// rows to NEITHER band → stale pixels at non-row-aligned splits.)
inline int bandRows(int scanY0, int scanY1, int rowLo, int rowHi, int* outLo, int* outHi)
{
    const int lo = std::max(rowLo, scanY0 / 8);
    const int hi = std::min(rowHi, (scanY1 + 7) / 8);
    *outLo = lo;
    *outHi = hi;
    return lo < hi ? 1 : 0;
}

inline int bandScanlines(int scanY0, int scanY1, int lineLo, int lineHi, int* outLo, int* outHi)
{
    const int lo = std::max(lineLo, scanY0);
    const int hi = std::min(lineHi, scanY1);
    *outLo = lo;
    *outHi = hi;
    return lo < hi ? 1 : 0;
}

} // namespace pom2::a2disp

#endif // POM2_APPLE2_DISPLAY_INTERNAL_H
