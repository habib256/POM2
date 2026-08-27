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

// LeChatMauve_ImGui — status + control panel for the Le Chat Mauve RGB
// video card. Read-only snapshot model (consistent with the Disk II panel):
// the host captures card state under EmulationController's stateMutex,
// hands a snapshot to render(), and dispatches the returned FrameResult
// actions back through the lock.

#ifndef POM2_LE_CHAT_MAUVE_IMGUI_H
#define POM2_LE_CHAT_MAUVE_IMGUI_H

#include "LeChatMauveCard.h"

namespace pom2 {

class LeChatMauve_ImGui
{
public:
    struct Snapshot {
        bool                        plugged         = false;
        int                         slot            = 7;      // actual bus slot (any 1-7 on //e)
        LeChatMauveCard::RenderMode mode            = LeChatMauveCard::RenderMode::COL140;
        uint8_t                     fifoBits        = 0b11;
        bool                        eightyCol       = false;   // current data line
        bool                        an3High         = false;   // current clock line
        bool                        invertBit7      = false;   // Dragon Wars compat
        bool                        colorTextEnable = true;    // Eve $C0B8/9
        bool                        hgrDuochrome    = false;   // Eve $C0BA/B
    };

    struct FrameResult {
        bool                        requestOverride        = false;
        LeChatMauveCard::RenderMode overrideTo             = LeChatMauveCard::RenderMode::COL140;
        bool                        requestReset           = false;
        bool                        requestInvertBit7      = false;
        bool                        invertBit7To           = false;
        bool                        requestColorTextEnable = false;
        bool                        colorTextEnableTo      = true;
        bool                        requestHgrDuochrome    = false;
        bool                        hgrDuochromeTo         = false;
    };

    FrameResult render(const char*     title,
                       bool&           open,
                       const Snapshot& snap);
};

} // namespace pom2

#endif // POM2_LE_CHAT_MAUVE_IMGUI_H
