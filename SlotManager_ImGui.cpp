// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlotManager_ImGui — see header. Pure data-in / actions-out renderer.

#include "SlotManager_ImGui.h"

#include "IconsFontAwesome6.h"
#include "SlotCardCatalog.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace pom2 {

namespace {

// Card types allowed in more than one slot at once. Disk II has always been
// multi-instance (option C); CFFA + SmartPort have per-slot persistence so
// they join it now that the Slot Manager can drive several of each.
bool isMultiInstance(const std::string& key)
{
    return key == "diskii" || key == "cffa" || key == "smartport35";
}

// Status dot, matching the colour scheme used by the other device panels:
// grey = empty, orange = mounted-but-write-protected, green = loaded.
void drawDot(const SlotManager_ImGui::BaySnapshot& b)
{
    ImVec4 col;
    if (!b.loaded)             col = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    else if (b.writeProtected) col = ImVec4(0.95f, 0.65f, 0.20f, 1.0f);
    else                       col = ImVec4(0.30f, 0.85f, 0.30f, 1.0f);
    ImGui::TextColored(col, ICON_FA_CIRCLE);
    ImGui::SameLine();
}

} // namespace

SlotManager_ImGui::Result
SlotManager_ImGui::render(const char* title, bool& open,
                          const PanelSnapshot& snap)
{
    Result r;
    for (int s = 0; s < kSlotCount; ++s) r.draftCards[s] = std::string();

    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    // Seed the local draft from the live snapshot the first time, or after
    // an Apply/Revert invalidated it.
    if (!draftValid_) {
        for (int s = 1; s <= 7; ++s) draft_[s] = snap.slots[s].cardKey;
        draftValid_ = true;
    }
    // Built-in slots are always forced to their plugged key — keep the draft
    // in sync so an Apply persists the locked value rather than a stale one.
    for (int s = 1; s <= 7; ++s)
        if (snap.slots[s].builtIn) draft_[s] = snap.slots[s].cardKey;

    ImGui::TextWrapped(
        "Assign a card to each expansion slot, then mount media per card "
        "below. Changing a card type needs an Apply (restarts the emulator); "
        "mounting / ejecting media is live.");
    ImGui::Spacing();

    // ── AUX 80-column row (IIe-class only, non-editable) ──────────────────
    if (snap.iieMode) {
        ImGui::BeginDisabled(true);
        ImGui::LabelText("AUX slot",
                         "Extended 80-Column Card (built-in, $C300 firmware)");
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    // ── Section 1: card assignment ────────────────────────────────────────
    ImGui::SeparatorText("Card assignment");

    auto isDuplicate = [&](int slot) -> bool {
        if (draft_[slot].empty())            return false;
        if (isMultiInstance(draft_[slot]))   return false;
        for (int s = 1; s <= 7; ++s)
            if (s != slot && draft_[s] == draft_[slot]) return true;
        return false;
    };

    bool anyDuplicate = false;
    for (int s = 1; s <= 7; ++s) {
        ImGui::PushID(s);
        char label[32];
        std::snprintf(label, sizeof(label), "Slot %d", s);

        const SlotSnapshot& ss = snap.slots[s];
        if (ss.builtIn) {
            char preview[112];
            std::snprintf(preview, sizeof(preview), "%s — %s",
                          cardLabelForKey(ss.cardKey), ss.builtInBadge.c_str());
            ImGui::BeginDisabled(true);
            ImGui::LabelText(label, "%s", preview);
            ImGui::EndDisabled();
            ImGui::PopID();
            continue;
        }

        const bool dup = isDuplicate(s);
        if (dup) anyDuplicate = true;

        const char* preview = cardLabelForKey(draft_[s]);
        if (dup) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
        if (ImGui::BeginCombo(label, preview)) {
            for (const auto& ct : kCardTypes) {
                const bool selected = (draft_[s] == ct.key);
                const bool disabled =
                    ((std::string(ct.key) == "mouse") && !snap.mouseAvailable) ||
                    ((std::string(ct.key) == "cffa")  && !snap.cffaAvailable);
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Selectable(ct.label, selected)) draft_[s] = ct.key;
                if (disabled) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(ROMs missing)");
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (dup) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (anyDuplicate) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "One card type per slot — fix duplicates above "
                           "(Disk II / CFFA / SmartPort may repeat).");
    }
    ImGui::BeginDisabled(anyDuplicate);
    if (ImGui::Button("Apply (restarts emulator)")) {
        r.applyAssignments = true;
        for (int s = 1; s <= 7; ++s) r.draftCards[s] = draft_[s];
        draftValid_ = false;  // re-seed from the post-restart snapshot
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) draftValid_ = false;

    // ── Section 2: media bays ─────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Media");

    bool anyMedia = false;
    for (int s = 1; s <= 7; ++s) {
        const SlotSnapshot& ss = snap.slots[s];
        if (!ss.occupied) continue;
        anyMedia = true;
        ImGui::PushID(1000 + s);

        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "Slot %d — %s%s", s,
                      ss.cardLabel.c_str(), ss.builtIn ? " (built-in)" : "");
        ImGui::TextUnformatted(hdr);

        if (ss.hasDetailPanel) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open detailed panel"))
                r.openDetailForSlot = s;
        }
        if (!ss.bays.empty() && ss.bays[0].loaded) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Boot slot")) r.bays[s][0].boot = true;
        }

        if (ss.bays.empty()) {
            ImGui::TextDisabled("    (no mountable media — use the detailed "
                                "panel)");
            ImGui::Separator();
            ImGui::PopID();
            continue;
        }

        const int nbays =
            ss.bays.size() < kMaxBays ? (int)ss.bays.size() : kMaxBays;
        for (int b = 0; b < nbays; ++b) {
            const BaySnapshot& bs = ss.bays[b];
            BayAction& act = r.bays[s][b];
            ImGui::PushID(b);
            ImGui::Indent();

            drawDot(bs);
            if (bs.supportsTypeSelect)
                ImGui::Text("Unit %d", b);
            else
                ImGui::Text("Image");
            // Kind / size summary.
            if (bs.loaded) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s, %u blocks%s)",
                    bs.kindLabel.empty() ? "media" : bs.kindLabel.c_str(),
                    bs.blockCount, bs.writeProtected ? ", WP" : "");
            } else if (!bs.kindLabel.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", bs.kindLabel.c_str());
            }

            // Type selector (SmartPort units only).
            if (bs.supportsTypeSelect) {
                const char* curLabel = "(empty)";
                for (const auto& opt : bs.typeOptions)
                    if (opt.first == bs.typeKey) { curLabel = opt.second.c_str(); break; }
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("Type", curLabel)) {
                    for (const auto& opt : bs.typeOptions) {
                        const bool sel = (opt.first == bs.typeKey);
                        if (ImGui::Selectable(opt.second.c_str(), sel)) {
                            if (opt.first != bs.typeKey) {
                                act.typeChanged = true;
                                act.newType     = opt.first;
                            }
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Path input — primed once from the mounted path.
            char* buf = pathBufs_[s][b].data();
            if (!pathPrimed_[s][b]) {
                std::snprintf(buf, pathBufs_[s][b].size(), "%s", bs.path.c_str());
                pathPrimed_[s][b] = true;
            }
            ImGui::SetNextItemWidth(320);
            ImGui::InputText("##path", buf, pathBufs_[s][b].size());

            const bool typeAllowsMount = !bs.supportsTypeSelect || !bs.typeKey.empty();
            ImGui::SameLine();
            ImGui::BeginDisabled(buf[0] == '\0' || !typeAllowsMount);
            if (ImGui::Button("Mount")) act.mountPath = buf;
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!bs.loaded);
            if (ImGui::Button("Eject")) act.eject = true;
            ImGui::EndDisabled();

            if (bs.supportsWriteBack) {
                bool wb = bs.writeBackEnabled;
                ImGui::BeginDisabled(!typeAllowsMount);
                if (ImGui::Checkbox("Write-back (save on eject)", &wb)) {
                    act.writeBackChanged = true;
                    act.writeBackOn      = wb;
                }
                ImGui::EndDisabled();
            }

            if (!bs.lastError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                   "Error: %s", bs.lastError.c_str());
            } else if (bs.loaded) {
                ImGui::TextDisabled("Mounted: %s", bs.path.c_str());
            } else {
                ImGui::TextDisabled("(no media)");
            }

            ImGui::Unindent();
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!anyMedia)
        ImGui::TextDisabled("No cards with mountable media are plugged.");

    ImGui::End();
    return r;
}

} // namespace pom2
