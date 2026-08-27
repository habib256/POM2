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

// StatusLed — one shared "media status" indicator for the slot-card panels.
// Before this, three panels (Slot Configuration, SmartPort, …) each rolled
// their own grey/yellow/green dot inline and none of them had a red "error"
// state. This unifies the colour scheme and adds the error case so a failed
// mount is visible at a glance ("lisibilité immédiate").
//
//   grey   = empty / no media
//   green  = loaded, writable
//   yellow = loaded, write-protected
//   red    = error (e.g. last mount attempt failed)
//
// Header-only: it's a thin wrapper over a coloured FontAwesome circle, used
// from several translation units, so there's nothing worth a .cpp.

#ifndef POM2_STATUS_LED_H
#define POM2_STATUS_LED_H

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace pom2 {

enum class MediaStatus { Empty, Ok, WriteProtected, Error };

inline ImVec4 statusLedColor(MediaStatus st)
{
    switch (st) {
        case MediaStatus::Ok:             return ImVec4(0.30f, 0.85f, 0.30f, 1.0f);
        case MediaStatus::WriteProtected: return ImVec4(0.95f, 0.65f, 0.20f, 1.0f);
        case MediaStatus::Error:          return ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
        case MediaStatus::Empty:
        default:                          return ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    }
}

inline const char* statusLedText(MediaStatus st)
{
    switch (st) {
        case MediaStatus::Ok:             return "Loaded";
        case MediaStatus::WriteProtected: return "Loaded (write-protected)";
        case MediaStatus::Error:          return "Error";
        case MediaStatus::Empty:
        default:                          return "Empty";
    }
}

inline MediaStatus mediaStatus(bool loaded, bool writeProtected, bool error = false)
{
    if (error)          return MediaStatus::Error;
    if (!loaded)        return MediaStatus::Empty;
    if (writeProtected) return MediaStatus::WriteProtected;
    return MediaStatus::Ok;
}

// Draw the dot and stay on the same line (the caller usually follows with a
// label). Hover shows `tip`, or a state-derived default when `tip` is null.
inline void statusLed(MediaStatus st, const char* tip = nullptr)
{
    ImGui::TextColored(statusLedColor(st), ICON_FA_CIRCLE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (tip && tip[0]) ? tip : statusLedText(st));
    ImGui::SameLine();
}

inline void statusLed(bool loaded, bool writeProtected, bool error = false,
                      const char* tip = nullptr)
{
    statusLed(mediaStatus(loaded, writeProtected, error), tip);
}

} // namespace pom2

#endif // POM2_STATUS_LED_H
