// POM2 Apple II Emulator
// Copyright (C) 2026
//
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
        bool                        plugged     = false;
        LeChatMauveCard::RenderMode mode        = LeChatMauveCard::RenderMode::COL140;
        uint8_t                     fifoBits    = 0b11;
        bool                        eightyCol   = false;   // current data line
        bool                        an3High     = false;   // current clock line
        bool                        invertBit7  = false;   // Dragon Wars compat
    };

    struct FrameResult {
        bool                        requestOverride     = false;
        LeChatMauveCard::RenderMode overrideTo          = LeChatMauveCard::RenderMode::COL140;
        bool                        requestReset        = false;
        bool                        requestInvertBit7   = false;
        bool                        invertBit7To        = false;
    };

    FrameResult render(const char*     title,
                       bool&           open,
                       const Snapshot& snap);
};

} // namespace pom2

#endif // POM2_LE_CHAT_MAUVE_IMGUI_H
