// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MainWindow_Panels — the panel registry's one binding table, and the four
// views derived from it.
//
// Before this file, a panel existed in six places at once: 32 lines to load
// its visibility, 32 to save it, 38 to offer it in the command palette, 38
// more to dispatch that command, 37 menu rows, and a 28-assignment block that
// hid "every" panel on the browser build. Nothing checked any of them against
// the others, and they had already drifted — seven panels the palette could
// open had no settings key at all, so they never came back after a restart.
//
// Now there is `PanelCatalog.h` (what a panel is), `registerPanels()` below
// (which `bool` it lives in, and what makes it available), and four short
// functions that DERIVE the menus, the palette, the palette's dispatch and the
// settings round-trip from those. Adding a panel is a catalog row plus a bind
// line; forgetting the bind is caught at startup by `unbound()`.
//
// It lives outside MainWindow.cpp for the reason the file-size ratchet exists:
// the god-object does not get to grow by 250 lines of table.

#include "MainWindow.h"

#include "EchoPlusCard.h"
#include "FujiNetCard.h"
#include "Logger.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "imgui.h"

#include <string>

namespace {

/// "(slot N)" for a plugged card, "(no card plugged)" for an empty bay. The
/// menus used to spell this out per panel, five times over, each slightly
/// differently.
template <typename Card>
std::string slotSuffix(Card* card)
{
    return card ? " (slot " + std::to_string(card->getSlot()) + ")"
                : std::string(" (no card plugged)");
}

}  // namespace

// ── The binding table ────────────────────────────────────────────────────

void MainWindow::registerPanels()
{
    auto card = [](auto** p) { return [p] { return *p != nullptr; }; };

    // File / Machine
    panels_.bind("panel.disklibrary", &showDiskLibrary);
    panels_.bind("panel.slotconfig",  &showSlotConfigPanel);

    // Devices ▸ Storage
    panels_.bind("panel.media",     &showMediaPanel);
    panels_.bind("panel.floppyemu", &showFloppyEmu);
    panels_.bind("panel.cassette",  &showCassetteDeck);
    panels_.bind("panel.diskii",    &showDiskPanel);
    // The 3.5" drive is a SmartPort card's on any //e, and the machine's own
    // on a //c+ — the label has to say which, because "Disk 3.5" alone leaves
    // the user hunting for a card that is not there.
    panels_.bind("panel.disk35", &showDisk35Panel, {}, [this] {
        return smartPortCard
            ? "Disk 3.5\" (slot " + std::to_string(smartPortCard->getSlot()) + ")"
            : std::string("Disk 3.5\" (//c+ on-board)");
    });
    panels_.bind("panel.hdv", &showHdvPanel, {}, [this] {
        return "HDV (slot " + std::to_string(hdvCard ? hdvCard->getSlot() : 5) + ")";
    });
    panels_.bind("panel.smartport", &showSmartPortPanel, card(&smartPortCard),
                 [this] {
                     return "SmartPort Configuration" + slotSuffix(smartPortCard);
                 });
    panels_.bind("panel.fujinet", &showFujiNetPanel, card(&fujiNetCard),
                 [this] { return "FujiNet" + slotSuffix(fujiNetCard); });

    // Devices ▸ Sound
    panels_.bind("panel.mockingboard", &showMockingboardPanel);
    panels_.bind("panel.phasor", &showPhasorPanel, card(&phasorCard),
                 [this] { return "Phasor" + slotSuffix(phasorCard); });
    panels_.bind("panel.echoplus", &showEchoPlusPanel, card(&echoPlusCard),
                 [this] { return "Echo+" + slotSuffix(echoPlusCard); });
    panels_.bind("panel.mixer", &showAudioMixer);

    // Devices ▸ Ports & cards
    // Super Serial is the one card a profile can carry TWICE (the //c's
    // printer and modem ports), so its label lists every slot it occupies.
    panels_.bind("panel.ssc", &showSscPanel,
                 [this] { return !sscCards.empty(); },
                 [this] {
                     if (sscCards.empty()) return std::string("Super Serial (no card plugged)");
                     if (sscCards.size() == 1)
                         return "Super Serial (slot " +
                                std::to_string(sscCards[0]->getSlot()) + ")";
                     std::string lbl = "Super Serial (slots";
                     for (size_t i = 0; i < sscCards.size(); ++i) {
                         lbl += (i == 0) ? " " : ", ";
                         lbl += std::to_string(sscCards[i]->getSlot());
                     }
                     return lbl + ")";
                 });
    // One entry covers both NICs; the panel tabs between whichever are in.
    panels_.bind("panel.ethernet", &showEthernetPanel,
                 [this] { return uthernetCard || uthernetIICard; },
                 [this] {
                     if (uthernetIICard && uthernetCard)
                         return "Ethernet (Uthernet I slot " +
                                std::to_string(uthernetCard->getSlot()) +
                                ", II slot " +
                                std::to_string(uthernetIICard->getSlot()) + ")";
                     if (uthernetIICard)
                         return "Ethernet (Uthernet II, slot " +
                                std::to_string(uthernetIICard->getSlot()) + ")";
                     if (uthernetCard)
                         return "Ethernet (Uthernet I, slot " +
                                std::to_string(uthernetCard->getSlot()) + ")";
                     return std::string("Ethernet (no card plugged)");
                 });
    panels_.bind("panel.printer", &showPrinterPanel, card(&printerCard),
                 [this] { return "Printer" + slotSuffix(printerCard); });
    // The ImageWriter is the PRINTER hanging off whichever interface card is
    // plugged, so it is always openable — an empty paper tray is a legitimate
    // thing to look at.
    panels_.bind("panel.imagewriter", &showImageWriterPanel);
    panels_.bind("panel.chatmauve",   &showChatMauvePanel);
    panels_.bind("panel.joystick",    &showJoystickPanel);
    panels_.bind("panel.keyboard",    &showKeyboardPanel);

    // Devices ▸ Inspectors & tools
    panels_.bind("panel.rewind",  &showRewindBar);
    panels_.bind("panel.mouse",   &showMouseInspector);
    panels_.bind("panel.nsclock", &showNoSlotClockPanel);

    // Display
    panels_.bind("panel.crt",      &showNtscSettings);
    panels_.bind("panel.voxel",    &show3dVoxel_);
    panels_.bind("panel.voxelset", &showVoxelSettings_);

    // View
    panels_.bind("panel.memviewer", &showMemViewer);
    panels_.bind("panel.debugger",  &showDebugger);
    panels_.bind("panel.membar",    &showMemoryBar);
    panels_.bind("panel.membarh",   &showMemoryBarH);
    panels_.bind("panel.memgrid",   &showMemoryGrid);

    // Tools
    panels_.bind("panel.hgrpaint",  &showHgrPaintEditor);
    panels_.bind("panel.hgrsprite", &showHgrSpriteEditor);
    panels_.bind("panel.aicontrol", &showAiControlPanel);
#ifdef __EMSCRIPTEN__
    // AiControlServer::start() cannot open a listening socket in the browser
    // sandbox, so the panel is absent rather than greyed: a greyed row says
    // "plug something in", and there is nothing to plug in.
    panels_.hideFromUi("panel.aicontrol");
#endif

    // Help
    panels_.bind("panel.welcome",     &showWelcomePanel);
    panels_.bind("panel.romstatus",   &showRomStatusPanel);
    panels_.bind("panel.abstraction", &showAbstractionPanel);

    // The two ways this table can be wrong, both silent until now: a catalog
    // row nobody bound (a menu entry that toggles nothing) and a bind for an
    // id that does not exist (a panel that never appears). Neither is worth
    // aborting over, both are worth a line in the log.
    for (const std::string& id : panels_.unbound())
        pom2::log().error("UI", "panel declared but never bound: " + id);
    for (const std::string& id : panels_.unknownBinds())
        pom2::log().error("UI", "panel bound but not in the catalog: " + id);
}

// ── View 1: menu rows ────────────────────────────────────────────────────

void MainWindow::panelMenuItem(const char* id)
{
    const pom2::PanelRegistry::Binding* b = panels_.find(id);
    if (!b || b->hidden) return;
    const bool enabled = panels_.available(*b);
    const std::string label = panels_.title(*b);

    if (!enabled) ImGui::BeginDisabled();
    ImGui::MenuItem(label.c_str(), b->info->shortcut, b->visible);
    if (!enabled) ImGui::EndDisabled();
    // AllowWhenDisabled so an unplugged card still explains what it would do
    // — the tooltip is how the user learns the panel exists before owning it.
    if (b->info->tip &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", b->info->tip);
}

void MainWindow::panelMenuGroup(pom2::PanelGroup group)
{
    for (const pom2::PanelRegistry::Binding& b : panels_.all())
        if (b.info->group == group) panelMenuItem(b.info->id);
}

// ── View 2 + 3: the command palette and its dispatch ─────────────────────

void MainWindow::forEachPanelCommand(
    const std::function<void(const char*, const std::string&, const char*, bool,
                             bool)>& add) const
{
    for (const pom2::PanelRegistry::Binding& b : panels_.all()) {
        if (b.hidden) continue;
        add(b.info->id, panels_.title(b),
            b.info->shortcut ? b.info->shortcut : "",
            panels_.available(b), b.visible && *b.visible);
    }
}

bool MainWindow::runPanelCommand(const std::string& id)
{
    return panels_.toggle(id);
}

// ── View 4: the settings round-trip ──────────────────────────────────────

void MainWindow::loadPanelVisibility()
{
    if (!settings) return;
    panels_.forEachPersisted([this](const char* key, bool* flag) {
        *flag = settings->getBool(key, *flag);
    });
}

void MainWindow::savePanelVisibility()
{
    if (!settings) return;
    panels_.forEachPersisted([this](const char* key, bool* flag) {
        settings->setBool(key, *flag);
    });
}

void MainWindow::hideAllPanels()
{
    panels_.hideAll();
}
