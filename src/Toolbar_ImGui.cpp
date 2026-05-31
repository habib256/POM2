// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Toolbar_ImGui.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace pom2 {

namespace {

// Convenience: icon + text fallback in one place. When `fa-solid-900.ttf`
// failed to load the icon glyph renders as `?`; the tooltip carries the
// long label either way.
struct Btn { const char* icon; const char* id; const char* tip; };

bool iconButton(const Btn& b, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    char label[64];
    std::snprintf(label, sizeof(label), "%s##%s", b.icon, b.id);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && b.tip) ImGui::SetTooltip("%s", b.tip);
    return clicked;
}

const char* profileShortLabel(SystemProfile p) {
    switch (p) {
        case SystemProfile::AppleII:             return "][";
        case SystemProfile::AppleIIPlus:         return "][+";
        case SystemProfile::AppleIIeUnenhanced:  return "//e-U";
        case SystemProfile::AppleIIe:            return "//e";
        case SystemProfile::AppleIIc:            return "//c";
        case SystemProfile::AppleIIcPlus:        return "//c+";
    }
    return "??";
}

// Compact label shown inside the toolbar combo when collapsed. Full
// names live in the dropdown rows (e.g. "//e/c — Français (342-0274-A)")
// but those wouldn't fit in the toolbar's ~110 px width.
const char* shortLocaleLabel(CharRomLocale l) {
    switch (l) {
        case CharRomLocale::ProfileDefault:                   return "Default";
        case CharRomLocale::AppleIIClassic:                   return "Classic";
        case CharRomLocale::AppleIIeUS_Enhanced:              return "US";
        case CharRomLocale::AppleIIeUS_Unenhanced:            return "US-U";
        case CharRomLocale::AppleIIeFrench:                   return "FR";
        case CharRomLocale::AppleIIeFrenchCanadian:           return "FR-CA";
        case CharRomLocale::AppleIIeFrenchCanadianUnenhanced: return "FR-CA-U";
        case CharRomLocale::AppleIIeUK_Enhanced:              return "UK";
        case CharRomLocale::AppleIIeUK_Unenhanced:            return "UK-U";
        case CharRomLocale::AppleIIeGerman:                   return "DE";
        case CharRomLocale::AppleIIeGermanImproved:           return "DE+";
    }
    return "?";
}

} // anon namespace

Toolbar_ImGui::Result Toolbar_ImGui::render(
    float /*unused*/, const Snapshot& snap)
{
    Result r;

    // Pin to the top-left, flush against the menu bar. `WorkPos`
    // already excludes the main menu bar (it's the top-left of the
    // viewport's work area), so we use it as-is — adding a
    // `menuBarHeight` offset on top pushed the toolbar one row too
    // low (fixed 2026-05-15). The `menuBarHeight` parameter is kept
    // for ABI stability in case the caller can't always feed
    // `WorkPos`; ignored here.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 0));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 4.0f));
    if (!ImGui::Begin("##POM2_Toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return r;
    }

    // ── Power group ───────────────────────────────────────────────────
    // Power-cycle (cold boot, wipes RAM) + soft reset (Ctrl-Reset).
    // Hard reset is keyboard-only (F12) — was redundant in the toolbar
    // (effectively the same UX as soft reset for most users, the
    // distinction matters for the few users who reach for it).
    if (iconButton({ ICON_FA_POWER_OFF,    "ColdBoot",
                     "Power-cycle (wipe RAM + cold boot)" })) {
        r.requestColdBoot = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_ROTATE_LEFT,  "SoftReset",
                     "Reset (Ctrl-Reset / F11)" })) {
        r.requestSoftReset = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Run group ─────────────────────────────────────────────────────
    // Rewind sits on the opposite side of Pause from Step: hold it to replay
    // backwards (same gesture as F6 / the Devices ▸ Rewind bar). Enabled only
    // when there's recorded history; iconButton can't report "held", so it's
    // drawn inline to read IsItemActive().
    {
        const bool canRewind = snap.rewindEnabled && snap.rewindHasFrames;
        ImGui::BeginDisabled(!canRewind);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s##RewindHold", ICON_FA_BACKWARD_FAST);
        ImGui::Button(lbl);
        if (ImGui::IsItemActive()) r.requestRewindHeld = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", canRewind
                ? "Hold to rewind (live) — also F6, or Devices \xe2\x96\xb8 Rewind"
                : "Rewind: turn on recording in Devices \xe2\x96\xb8 Rewind");
        ImGui::SameLine();
    }

    const char* runIcon = snap.isRunning
        ? ICON_FA_CIRCLE_PAUSE : ICON_FA_CIRCLE_PLAY;
    const char* runTip  = snap.isRunning
        ? "Pause (CPU stops at next instruction boundary)"
        : "Run (resume CPU from current PC)";
    if (iconButton({ runIcon, "PauseToggle", runTip })) {
        r.requestPauseToggle = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_FORWARD_STEP, "Step",
                     "Single-instruction step (only useful when paused)" },
                   !snap.isRunning)) {
        r.requestStep = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Speed selector ───────────────────────────────────────────────
    // Combo: 1× / 2× / 4× / MAX. The current speed sticks to whichever
    // bucket the cyclesPerFrame value rounds into; off-bucket values
    // (the user typed a custom speed) read as
    // "custom" with no checkmark.
    static constexpr int kSpeed1x  = 17045;
    static constexpr int kSpeed2x  = 17045 * 2;
    static constexpr int kSpeed4x  = 17045 * 4;
    static constexpr int kSpeedMax = 1'000'000;
    const char* speedLabel = "Speed";
    if      (snap.cyclesPerFrame == kSpeed1x)  speedLabel = ICON_FA_GAUGE_SIMPLE " 1×";
    else if (snap.cyclesPerFrame == kSpeed2x)  speedLabel = ICON_FA_GAUGE      " 2×";
    else if (snap.cyclesPerFrame == kSpeed4x)  speedLabel = ICON_FA_GAUGE_HIGH " 4×";
    else if (snap.cyclesPerFrame == kSpeedMax) speedLabel = ICON_FA_BOLT       " MAX";
    else                                        speedLabel = ICON_FA_GAUGE      " …";
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::BeginCombo("##POM2ToolbarSpeed", speedLabel)) {
        if (ImGui::Selectable(ICON_FA_GAUGE_SIMPLE " 1× (1.02 MHz)",
                              snap.cyclesPerFrame == kSpeed1x))
            r.setCyclesPerFrame = kSpeed1x;
        if (ImGui::Selectable(ICON_FA_GAUGE      " 2× (2.05 MHz)",
                              snap.cyclesPerFrame == kSpeed2x))
            r.setCyclesPerFrame = kSpeed2x;
        if (ImGui::Selectable(ICON_FA_GAUGE_HIGH " 4× (4.09 MHz)",
                              snap.cyclesPerFrame == kSpeed4x))
            r.setCyclesPerFrame = kSpeed4x;
        if (ImGui::Selectable(ICON_FA_BOLT       " MAX (uncapped)",
                              snap.cyclesPerFrame == kSpeedMax))
            r.setCyclesPerFrame = kSpeedMax;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emulation speed");

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Profile selector ─────────────────────────────────────────────
    // Short-label combo. Switching = full cold reset under the hood
    // (the host calls `applyProfile()`), so the user picks rarely.
    char profileBuf[16];
    std::snprintf(profileBuf, sizeof(profileBuf),
                  ICON_FA_COMPUTER " %s", profileShortLabel(snap.activeProfile));
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::BeginCombo("##POM2ToolbarProfile", profileBuf)) {
        for (SystemProfile p : pom2::allProfiles()) {
            char rowBuf[32];
            std::snprintf(rowBuf, sizeof(rowBuf), "%s  (%s)",
                          profileShortLabel(p),
                          std::string(pom2::profileConfig(p).displayName).c_str());
            if (ImGui::Selectable(rowBuf, snap.activeProfile == p)) {
                r.setProfileRequested = true;
                r.setProfile          = p;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("System profile (cold-reset switch)");

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Char ROM (locale) selector ───────────────────────────────────
    // Hot swap: changing the locale calls Memory::loadCharRom(path)
    // immediately — no profile-switch / cold-reset. Apple2Display
    // re-reads `mem.charRom()` on every frame so the new glyph table
    // shows up at the next refresh. The dropdown filters entries by
    // active profile so a II+ user doesn't see a German //e font
    // (which would render as garbage on a 2 KB-expecting profile).
    {
        const CharRomEntry& active = charRomEntry(snap.charRomLocale);
        char buf[40];
        std::snprintf(buf, sizeof(buf), ICON_FA_FONT " %s",
                      snap.charRomLocale == CharRomLocale::ProfileDefault
                          ? "Default"
                          : shortLocaleLabel(snap.charRomLocale));
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##POM2ToolbarCharRom", buf)) {
            for (const auto& e : charRomCatalog()) {
                if (!charRomFitsProfile(e, snap.activeProfile)) continue;
                if (ImGui::Selectable(e.displayName,
                                      snap.charRomLocale == e.locale)) {
                    r.setCharRomRequested = true;
                    r.setCharRomLocale    = e.locale;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            const char* path = active.path[0] ? active.path
                                              : "(profile probe order)";
            ImGui::SetTooltip("Character-generator ROM\n%s\n→ %s",
                              active.displayName, path);
        }
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Disk shortcuts ───────────────────────────────────────────────
    // Both insert AND eject-all now live in the Disk Library panel
    // (multi-source picker with boot-on-click + header-row "Eject All")
    // — kept out of the toolbar to avoid two ways to do the same thing.

    // ── Display color / monochrome toggle ────────────────────────────
    // One-click flip between color and B&W phosphor. The host remembers
    // the specific submode on each side (e.g. Mono Green ↔ Color 4-bit).
    // Tinted active when a mono mode is showing.
    if (snap.displayIsMono) {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    }
    if (iconButton({ ICON_FA_CIRCLE_HALF_STROKE, "MonoColorToggle",
                     snap.displayIsMono
                         ? "Monochrome — click for color"
                         : "Color — click for black & white" })) {
        r.requestMonoColorToggle = true;
    }
    if (snap.displayIsMono) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Tooling ──────────────────────────────────────────────────────
    if (iconButton({ ICON_FA_CAMERA,       "Screenshot",
                     "Save screenshot to ./screenshot_NNN.ppm  (F9)" })) {
        r.requestScreenshot = true;
    }
    ImGui::SameLine();
    {
        // Memory Map Grid is a toggle — render the icon with a light tint
        // when the grid is currently visible so the state is obvious
        // at a glance.
        if (snap.memoryGridVisible) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }
        if (iconButton({ ICON_FA_BORDER_ALL, "MemGrid",
                         snap.memoryGridVisible
                             ? "MemoryGrid viewer (visible — click to hide)"
                             : "MemoryGrid viewer (click to show)" })) {
            r.requestMemoryGridToggle = true;
        }
        if (snap.memoryGridVisible) ImGui::PopStyleColor();
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
    if (iconButton({ ICON_FA_CIRCLE_INFO, "About",
                     "About POM2" })) {
        r.requestAbout = true;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    return r;
}

} // namespace pom2
