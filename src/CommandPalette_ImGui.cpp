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

#include "CommandPalette_ImGui.h"

#include "Pom2Theme.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace pom2 {

namespace {

bool isWordBoundary(char prev)
{
    return prev == ' ' || prev == ':' || prev == '-' || prev == '/' ||
           prev == '(' || prev == '.' || prev == 0;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

} // anon namespace

int fuzzyScore(const char* needle, const char* hay)
{
    if (!needle || !*needle) return 0;    // empty query matches everything
    if (!hay) return -1;

    int  score      = 0;
    int  streak     = 0;
    char prev       = 0;
    const char* n   = needle;
    const char* h   = hay;

    while (*n && *h) {
        if (lower(*n) == lower(*h)) {
            score += 10;
            // A match starting a word is what the user almost certainly meant
            // ("mb" should find "Mockingboard (VIA + AY state)" via M and B of
            // two words, not via the "m" and "b" buried mid-token).
            if (isWordBoundary(prev)) score += 15;
            // Consecutive runs matter more than scattered hits: "mock" should
            // beat a string that merely happens to contain m, o, c, k.
            streak += 1;
            score  += streak * 8;
            ++n;
        } else {
            streak = 0;
            // Small penalty per skipped character so shorter, tighter matches
            // outrank long ones. Floored below so a long label can't go
            // negative and be mistaken for "no match".
            score -= 1;
        }
        prev = *h;
        ++h;
    }

    if (*n) return -1;                    // needle not fully consumed
    return std::max(0, score);
}

void CommandPalette_ImGui::open()
{
    if (!open_) {
        query_[0] = '\0';
        selected_ = 0;
    }
    open_        = true;
    focusQueued_ = true;
}

void CommandPalette_ImGui::close()
{
    open_ = false;
}

CommandPalette_ImGui::Result CommandPalette_ImGui::render()
{
    Result res;
    if (!open_) return res;

    const Palette& pal = palette();
    const auto     u32 = ImGui::ColorConvertU32ToFloat4;

    // Centred near the top: a palette is a transient overlay, and anchoring it
    // to the vertical centre would put it over whatever the user is looking at.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width  = std::min(640.0f * ImGui::GetFontSize() / 14.0f,
                                  vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.12f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();

    // NoDocking: it is an overlay, not a panel — being dockable would let it be
    // stranded as a tab somewhere. NoSavedSettings for the same reason.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    if (!ImGui::Begin("##POM2_CommandPalette", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return res;
    }

    // ── Query field ──────────────────────────────────────────────────────
    if (focusQueued_) {
        ImGui::SetKeyboardFocusHere();
        focusQueued_ = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    // EnterReturnsTrue is NOT used: Enter is handled below alongside the arrow
    // keys so the selected row wins, not the raw text.
    ImGui::InputTextWithHint("##palette_query", "Type a command...",
                             query_, sizeof(query_));
    const bool queryActive = ImGui::IsItemActive();

    // ── Match + rank ─────────────────────────────────────────────────────
    matches_.clear();
    {
        // Score against "Category Label" so typing "devices mock" works and so
        // a category name alone lists its commands.
        std::vector<std::pair<int, int>> scored;   // (score, index)
        scored.reserve(commands_.size());
        std::string hay;
        for (int i = 0; i < static_cast<int>(commands_.size()); ++i) {
            const Command& c = commands_[i];
            hay = c.category;
            if (!hay.empty()) hay += ' ';
            hay += c.label;
            const int s = fuzzyScore(query_, hay.c_str());
            if (s >= 0) scored.emplace_back(s, i);
        }
        // Stable sort by score desc, then by original order so an empty query
        // shows the host's list in the order it was built (menus first, etc.).
        std::stable_sort(scored.begin(), scored.end(),
                         [](const auto& a, const auto& b) {
                             return a.first > b.first;
                         });
        for (const auto& [s, i] : scored) matches_.push_back(i);
    }

    const int matchCount = static_cast<int>(matches_.size());
    if (matchCount == 0) selected_ = 0;
    else                 selected_ = std::clamp(selected_, 0, matchCount - 1);

    // ── Keyboard ─────────────────────────────────────────────────────────
    // A single-line InputText does not consume Up/Down, so reading them here is
    // safe and keeps focus in the field while the user moves the selection.
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && matchCount > 0)
        selected_ = (selected_ + 1) % matchCount;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && matchCount > 0)
        selected_ = (selected_ + matchCount - 1) % matchCount;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open_ = false;
        ImGui::End();
        ImGui::PopStyleVar();
        return res;
    }
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);

    ImGui::Spacing();

    // ── Results ──────────────────────────────────────────────────────────
    // Height follows the match count, capped at 10 rows. Resizing as you type
    // is safe *because the window is anchored at the top*: it grows and shrinks
    // downwards, so the query field the user is typing into never moves. (A
    // centre-anchored palette would need a fixed height instead.)
    const float rowH   = ImGui::GetTextLineHeightWithSpacing();
    const int   rows   = std::clamp(matchCount, 1, 10);
    const float listH  = rowH * static_cast<float>(rows);
    int         picked = -1;

    if (ImGui::BeginChild("##palette_list", ImVec2(0.0f, listH), false,
                          ImGuiWindowFlags_NoSavedSettings)) {
        for (int row = 0; row < matchCount; ++row) {
            const Command& c = commands_[matches_[row]];
            const bool isSel = (row == selected_);

            ImGui::PushID(row);
            if (!c.enabled) ImGui::BeginDisabled();

            // Selectable spans the row so the whole line is a click target.
            if (ImGui::Selectable("##row", isSel,
                                  ImGuiSelectableFlags_AllowOverlap |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (c.enabled) { selected_ = row; picked = matches_[row]; }
            }
            ImGui::SameLine(0.0f, 0.0f);

            // Check column — fixed width so labels line up whether or not a
            // command is a toggle.
            const float checkW = ImGui::CalcTextSize("* ").x;
            if (c.checked) {
                ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.accent));
                ImGui::TextUnformatted("*");
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::Dummy(ImVec2(checkW - ImGui::CalcTextSize("*").x, 0.0f));
            } else {
                ImGui::Dummy(ImVec2(checkW, 0.0f));
            }
            ImGui::SameLine(0.0f, 0.0f);

            if (!c.category.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
                ImGui::Text("%s  ", c.category.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::TextUnformatted(c.label.c_str());

            if (!c.shortcut.empty()) {
                const float w = ImGui::CalcTextSize(c.shortcut.c_str()).x;
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > w) {
                    ImGui::SameLine(0.0f, avail - w);
                    ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
                    ImGui::TextUnformatted(c.shortcut.c_str());
                    ImGui::PopStyleColor();
                }
            }

            if (!c.enabled) ImGui::EndDisabled();
            ImGui::PopID();

            // Keep the keyboard selection in view when it moves past the edge.
            if (isSel && (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ||
                          ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)))
                ImGui::SetScrollHereY(0.5f);
        }
        if (matchCount == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
            ImGui::TextUnformatted("No match.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    if (enter && matchCount > 0) {
        const Command& c = commands_[matches_[selected_]];
        if (c.enabled) picked = matches_[selected_];
    }

    // Click-outside dismiss. `queryActive` is not enough on its own — the user
    // may be scrolling the list, which also takes focus off the field.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !queryActive)
        open_ = false;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
    ImGui::TextUnformatted("Enter to run  ·  Up/Down to move  ·  Esc to close");
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();

    if (picked >= 0) {
        res.executed  = true;
        res.commandId = commands_[picked].id;
        open_         = false;
    }
    return res;
}

} // namespace pom2
