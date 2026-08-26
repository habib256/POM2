// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"
#include "MainWindowUiState.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "AudioCoordinator.h"
#include "DebugCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "MouseCoordinator.h"
#include "NetworkCoordinator.h"
#include "PrinterCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "StorageCoordinator.h"
#include "AtomicFileReplace.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CassetteDeck_ImGui.h"
#include "Rewind_ImGui.h"
#include "CassetteDevice.h"
#include "CharRomCatalog.h"
#include "SoftCardZ80.h"
#include "GrapplerCard.h"
#include "NetworkBackend.h"
#include "SlirpNetworkBackend.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "W5100HostSockets.h"
#include "FujiNetCard.h"
#include "FujiNetHost.h"
#include "Uthernet_ImGui.h"
#include "ImageWriter.h"
#include "ImageWriter_ImGui.h"
#include "RomStatus_ImGui.h"
#include "AbstractionLevels_ImGui.h"
#include "Keyboard_ImGui.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "Disk35Controller_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "Logger.h"
#include "MemoryViewer_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "MouseGrab.h"
#include "NtscPostProcessor.h"
#include "CrtEffectStack.h"
#include "Voxel3DRenderer.h"
#include "CffaCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "IconsFontAwesome6.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "PrinterScreenDump.h"
#include "PrinterHistory.h"
#include "PrinterSoundDevice.h"
#include "SmartPort_ImGui.h"
#include "FujiNet_ImGui.h"
#include "SpeakerDevice.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SpOverSlipLink.h"
#include "Ssi263.h"
#include "SystemProfile.h"
#include "Toolbar_ImGui.h"
#include "CommandPalette_ImGui.h"

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar (status bar)
#include "Pom2GL.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

// Physical MainWindow split: network, printer, audio, video and expansion-device panels.

void MainWindow::renderEthernetPanelWindow()
{
    if (!uiState_->showEthernetPanel) return;
    if (!ethernetPanel) ethernetPanel = std::make_unique<pom2::Uthernet_ImGui>();

    const auto snap = devicePanelCoordinator_->captureEthernet();

    const auto action =
        ethernetPanel->render("Ethernet###ethernetPanel", uiState_->showEthernetPanel, snap);

    devicePanelCoordinator_->applyEthernet(action);
}

void MainWindow::renderSscPanelWindow()
{
    if (!uiState_->showSscPanel) return;
    const auto serialCards = devicePanelCoordinator_->captureSerialCards();
    if (serialCards.empty()) return;
    pom2::DevicePanelCoordinator::SerialCommand command;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Super Serial###sscPanel", &uiState_->showSscPanel)) {
        ImGui::End();
        return;
    }

    // One panel hosts every plugged SSC under a tab bar. //c boots with
    // two (printer + modem); other profiles typically run zero or one.
    // Per-slot port-input state lives in a static map so each tab keeps
    // its own draft port number across frames.
    auto renderOne = [&](const pom2::DevicePanelCoordinator::SerialSnapshot& ssc) {
        const int slot = ssc.slot;
        static std::map<int, int> portDrafts;
        auto it = portDrafts.find(slot);
        if (it == portDrafts.end()) {
            portDrafts[slot] = ssc.port ? ssc.port
                : SuperSerialCard::kDefaultPort;
            it = portDrafts.find(slot);
        }
        int& portDraft = it->second;

        const bool listening = ssc.listening;
        const bool connected = ssc.connected;

        ImGui::Text("Status: %s%s",
            listening ? "listening" : "stopped",
            connected ? " — client connected" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("(slot %d)", slot);

        ImGui::Separator();
        ImGui::PushID(slot);
#ifdef __EMSCRIPTEN__
        // The host telnet bridge is compiled out under WASM
        // (SuperSerialCard::startListening() is a no-op — no TCP sockets in
        // the browser sandbox, see SuperSerialCard.cpp). The ACIA itself is
        // still emulated, so PR#n output still flows into the TX counter /
        // recent traffic below. Grey out the listener controls rather than
        // hide them, so the panel reads honestly.
        (void)listening; (void)connected;
        ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        ImGui::SameLine();
        ImGui::Button("Start listener");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Telnet bridge unavailable in the browser build.");
#else
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        if (portDraft < 1)     portDraft = 1;
        if (portDraft > 65535) portDraft = 65535;

        ImGui::SameLine();
        if (!listening) {
            if (ImGui::Button("Start listener")) {
                command.slot = slot;
                command.port = static_cast<uint16_t>(portDraft);
                command.requestStart = true;
            }
        } else {
            if (ImGui::Button("Stop listener")) {
                command.slot = slot;
                command.requestStop = true;
            }
        }

        if (listening) {
            ImGui::TextWrapped("Connect from a host terminal:");
            ImGui::TextWrapped("  telnet 127.0.0.1 %d", ssc.port);
            ImGui::TextWrapped("In the Apple II:  PR#%d  (or IN#%d for input)",
                slot, slot);
        } else {
            ImGui::TextDisabled("Click Start, then telnet to the port to "
                                "bridge I/O between your host shell and "
                                "the Apple II.");
        }
#endif

        ImGui::Separator();
        bool raw = ssc.rawMode;
        if (ImGui::Checkbox("Raw mode (8-bit binary)", &raw)) {
            command.slot = slot;
            command.requestRawMode = true;
            command.rawMode = raw;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: stock telnet — IAC ($FF) negotiation\n"
                              "swallowed + CR/LF normalised to CR.\n"
                              "On: every byte forwarded verbatim. Use for\n"
                              "XMODEM / Kermit / ADTPro / any binary protocol.");
        }

        bool tap = ssc.printerTap;
        if (ImGui::Checkbox("Feed ImageWriter printer", &tap)) {
            command.slot = slot;
            command.requestPrinterTap = true;
            command.printerTap = tap;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mirror every transmitted byte into the\n"
                              "host-side ImageWriter II (the //c's real\n"
                              "printer port is this serial card — slot 1\n"
                              "taps by default). A plugged parallel\n"
                              "Printer card or Grappler+ takes priority.");
        }

        ImGui::Separator();
        ImGui::Text("RX (telnet → A2): %llu B",
            static_cast<unsigned long long>(ssc.bytesRx));
        ImGui::Text("TX (A2 → telnet): %llu B",
            static_cast<unsigned long long>(ssc.bytesTx));

        if (ImGui::CollapsingHeader("Recent traffic")) {
            ImGui::TextDisabled("Last bytes the Apple II printed via PR#%d:",
                                slot);
            ImGui::TextWrapped("%s", ssc.recentTxText.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Last bytes the host typed:");
            ImGui::TextWrapped("%s", ssc.recentRxText.c_str());
        }
        ImGui::PopID();
    };

    if (serialCards.size() == 1) {
        renderOne(serialCards[0]);
    } else if (ImGui::BeginTabBar("##sscTabs")) {
        // //c convention: sl1 = printer port, sl2 = modem port. Other
        // profiles just label by slot number.
        const bool isIIcLayout = (serialCards.size() == 2) &&
            (serialCards[0].slot == 1) && (serialCards[1].slot == 2);
        for (size_t i = 0; i < serialCards.size(); ++i) {
            const int slot = serialCards[i].slot;
            std::string tab;
            if (isIIcLayout) tab = (i == 0) ? "Printer port (sl1)"
                                            : "Modem port (sl2)";
            else             tab = "Slot " + std::to_string(slot);
            if (ImGui::BeginTabItem(tab.c_str())) {
                renderOne(serialCards[i]);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    const auto result = devicePanelCoordinator_->applySerial(command);
    if (result.startAttempted && !result.startSucceeded) {
        uiState_->tapeStatusMessage = "SSC slot " + std::to_string(command.slot) +
            ": bind failed (port busy?)";
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
}

void MainWindow::renderPrinterPanelWindow()
{
    if (!uiState_->showPrinterPanel) return;
    auto printer = printerCoordinator_->capturePrinterPanel(*controller);
    if (!printer.plugged) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    const std::string title = "Printer (slot " +
        std::to_string(printer.slot) + ")###printerPanel";
    if (!ImGui::Begin(title.c_str(), &uiState_->showPrinterPanel)) {
        ImGui::End();
        return;
    }

    const size_t nBytes = printer.bytesWritten;
    ImGui::Text("Spool: %zu byte%s", nBytes, nBytes == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("— PR#%d from BASIC sends output here",
                        printer.slot);
    if (printer.spoolTruncated) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
            "Preview/save retains only the newest %zu bytes; older output "
            "was already streamed to the virtual printer.",
            PrinterCard::kMaxSpoolBytes);
    }

    ImGui::Separator();

    // Auto-suggest a timestamped path on first open so the user can hit
    // Save without typing anything. uiState_->printerSavePath persists across saves
    // within a session — the user can edit it freely.
    if (uiState_->printerSavePath.empty()) {
        const std::time_t t = std::time(nullptr);
        // localtime_r, not localtime: the latter hands back a shared static
        // `tm`, and ClockCard converts times of its own on the CPU thread (a
        // ProDOS timestamp under stateMutex). Same split as NoSlotClock.
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        uiState_->printerSavePath = (pom2::userDataDir() / "printouts" /
                           (std::string("spool-") + stamp + ".txt")).string();
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", uiState_->printerSavePath.c_str());
#ifdef __EMSCRIPTEN__
    // No persistent host filesystem in the browser build: a "Save as .txt"
    // would land in MEMFS and vanish on reload, with no download path. Grey
    // the controls out — the live spool preview below still works (PR#n
    // output is captured), and "Clear spool" still resets the buffer.
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-110);
    ImGui::InputText("##printerPath", buf, sizeof(buf));
    ImGui::SameLine();
    ImGui::Button("Save as .txt", ImVec2(100, 0));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Saving to a file is unavailable in the browser build.");
#else
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("##printerPath", buf, sizeof(buf))) {
        uiState_->printerSavePath = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as .txt", ImVec2(100, 0))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::path(uiState_->printerSavePath);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        const fs::path tmp = p.string() + ".pom2tmp";
        std::error_code permEc;
        const fs::perms oldPerms = fs::status(p, permEc).permissions();
        const bool havePerms = !permEc;
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            uiState_->printerLastSaveStatus = "Save failed: cannot open " +
                                    p.string();
        } else {
            const std::string& text = printer.spoolText;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            out.close();
            if (!out) {
                uiState_->printerLastSaveStatus = "Save failed: short write to " +
                                        tmp.string();
                fs::remove(tmp, ec);
            } else {
                if (havePerms) {
                    fs::permissions(tmp, oldPerms, ec);
                    ec.clear();
                }
                if (!pom2::replaceFileAtomic(tmp, p, ec)) {
                    uiState_->printerLastSaveStatus = "Save failed: cannot replace " +
                                            p.string() + ": " + ec.message();
                    fs::remove(tmp, ec);
                } else {
                    uiState_->printerLastSaveStatus = "Saved " +
                        std::to_string(text.size()) + " bytes → " + p.string();
                }
            }
        }
    }

    if (!uiState_->printerLastSaveStatus.empty()) {
        ImGui::TextDisabled("%s", uiState_->printerLastSaveStatus.c_str());
    }
#endif

    if (ImGui::Button("Clear spool")) {
        (void)printerCoordinator_->clearPrinterPanelSpool(
            *controller, printer.slot);
        printer.spoolText.clear();
        printer.bytesWritten = 0;
        uiState_->printerLastSaveStatus.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(host-side buffer only — does NOT touch the Apple II)");

    ImGui::Separator();
    ImGui::TextDisabled("Preview (high bit stripped, CR → LF):");

    // The coordinator copied this once under the state lock. Rendering and
    // host file I/O never retain or dereference the SlotBus-owned card.
    const std::string& preview = printer.spoolText;
    ImGui::BeginChild("##printerPreview", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (preview.empty()) {
        ImGui::TextDisabled("(empty — try `PR#%d : PRINT \"HELLO\"` from BASIC)",
                            printer.slot);
    } else {
        ImGui::TextUnformatted(preview.data(),
                               preview.data() + preview.size());
    }
    ImGui::EndChild();

    ImGui::End();
}

void MainWindow::pumpImageWriter()
{
    // The printer is downstream of the interface card: every byte the card
    // spooled since the last frame is handed to the ImageWriter, in order.
    // Cheap enough to run unconditionally (the panel need not be open —
    // a printout should be waiting when the user opens the tray).
    if (!imageWriter) return;

    // Archive first, so this runs on EVERY path — the "no source this frame"
    // branch below returns early, and a job already inside the printer's
    // buffer keeps ejecting sheets after its card is unplugged. Running at
    // the top costs one frame of lag and never misses a page.
    archiveNewPrinterPages();

    // Resolve and drain one source while holding the machine lock. The batch
    // is an owned value; the slower host-side mechanism runs after release.
    auto batch = printerCoordinator_->drainImageWriter(*controller);
    if (!batch.haveSource()) {
        // The mechanism keeps running: a job already in the printer's
        // input buffer must still reach paper even with nothing feeding
        // it (unplugging the card does not un-print the page).
        imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));
        return;
    }

    if (!batch.bytes.empty()) {
        imageWriter->queueBytes(batch.bytes.data(), batch.bytes.size());
        if (imageWriter->tracing())
            imageWriter->traceEvent("card delivered %zu byte%s (queue now %zu)",
                                    batch.bytes.size(),
                                    batch.bytes.size() == 1 ? "" : "s",
                                    imageWriter->pendingBytes());
    }

    // Print what the mechanism had time for this frame. ImGui's DeltaTime
    // is the host frame time, which is what a real print head answers to
    // (the guest can be paused, turbo'd or rewound — the paper still
    // moves at 250 cps).
    imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));

    // Report the printer's input buffer back up the cable. A stock
    // ImageWriter II buffers 2 KB and stops acknowledging bytes until the
    // head catches up; the Grappler firmware spins on that ACK bit before
    // every byte, so a guest printing a long job now waits for the paper
    // instead of blasting a whole page into a host queue.
    // Only hold the line when the user asked for the real handshake. The
    // coordinator re-resolves Grappler+ and performs this atomic line update
    // without exposing the card to the render loop.
    const bool busy =
        uiState_->printerBackPressure &&
        imageWriter->pendingBytes() > pom2::ImageWriter::kInputBufferBytes;
    const auto update =
        printerCoordinator_->setGrapplerBusy(*controller, busy);
    if (update.changed && imageWriter->tracing()) {
        imageWriter->traceEvent(
            "BUSY=%d — the Apple II %s (queue %zu / %zu buffer)",
            busy ? 1 : 0,
            busy ? "is now waiting on the printer" : "may send again",
            imageWriter->pendingBytes(),
            pom2::ImageWriter::kInputBufferBytes);
    }
}

void MainWindow::renderImageWriterWindow()
{
    if (!uiState_->showImageWriterPanel || !imageWriter || !imageWriterPanel) return;

    pom2::ImageWriter_ImGui::HostInfo host;

    // One locked value snapshot describes both the selected cable source and
    // every lower-priority interface that is deliberately not feeding it.
    const auto printerHost = printerCoordinator_->captureHost(*controller);
    using Source = pom2::PrinterCoordinator::SourceKind;
    switch (printerHost.source) {
        case Source::PrinterCard:
            host.haveSource = true;
            host.sourceLabel = "fed by Printer card, slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case Source::Grappler:
            host.haveSource = true;
            host.sourceLabel = "fed by Grappler+, slot " +
                               std::to_string(printerHost.sourceSlot);
        // The Grappler's S1 printer-type switches decide which dialect of
        // escape codes its firmware emits — an Epson-configured card feeds
        // this ImageWriter Epson graphics commands, which come out as
        // noise. Surface it where the damage shows up.
        using PT = GrapplerCard::PrinterType;
        for (int i = 0; i <= 6; ++i) {
            const auto t = static_cast<PT>(i);
            host.cardDipOptions.push_back(
                { GrapplerCard::printerTypeName(t), i });
        }
        host.cardDipValue = printerHost.grapplerPrinterType;
        host.cardDipRecommended = static_cast<int>(PT::AppleDotMatrix);
        host.backPressure = uiState_->printerBackPressure;
        host.onBackPressureChanged =
            [this](bool v) { uiState_->printerBackPressure = v; };
        host.onCardDipChanged = [this](int v) {
            (void)printerCoordinator_->setGrapplerPrinterType(*controller, v);
        };
            break;
        case Source::FujiNet:
            host.haveSource = true;
            host.sourceLabel = "fed by FujiNet printer, slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case Source::SuperSerial:
            host.haveSource = true;
            host.sourceLabel = "fed by Super Serial (printer port), slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case Source::None:
            break;
    }
    if (host.haveSource && !printerHost.ignoredSources.empty()) {
        host.sourceLabel += "  (not feeding: ";
        for (size_t i = 0; i < printerHost.ignoredSources.size(); ++i)
            host.sourceLabel +=
                (i ? ", " : "") + printerHost.ignoredSources[i];
        host.sourceLabel += ")";
    }
    host.saveDir = (pom2::userDataDir() / "printouts").string();

    // ── Print history (printer plan phase E) ─────────────────────────────
    // The panel lists and asks; the store stays here. Rebuilt each frame,
    // which is cheap — it is a few dozen rows of already-loaded metadata.
    if (printerHistory && printerHistory->isOpen()) {
        std::string writeErr;
        if (!printerHistory->pollWriteFailures(writeErr))
            pom2::log().warn("PrinterHistory", writeErr);
        host.historyDir = printerHistory->dir();
        host.history.reserve(printerHistory->size());
        for (const auto& p : printerHistory->pages()) {
            pom2::ImageWriter_ImGui::HostInfo::HistoryRow row;
            row.file    = p.file;
            row.savedAt = p.savedAt;
            row.printer = pom2::ImageWriter::modelName(
                static_cast<pom2::IwModel>(p.model));
            row.ribbon  = pom2::ImageWriter::ribbonName(
                static_cast<pom2::ImageWriter::Ribbon>(p.ribbon));
            row.job     = p.job;
            row.w       = p.w;
            row.h       = p.h;
            row.paperW  = p.paperW;
            row.paperL  = p.paperL;
            host.history.push_back(std::move(row));
        }
        host.loadHistoryPage = [this](const std::string& file,
                                      std::vector<uint8_t>& rgba,
                                      int& w, int& h) {
            for (const auto& p : printerHistory->pages()) {
                if (p.file != file) continue;
                std::string err;
                return printerHistory->loadRgba(p, rgba, w, h, err);
            }
            return false;
        };
        host.onDeleteHistoryPage = [this](const std::string& file) {
            for (const auto& p : printerHistory->pages()) {
                if (p.file != file) continue;
                std::string err;
                if (!printerHistory->erase(p, err))
                    pom2::log().warn("PrinterHistory", err);
                return;
            }
        };
        host.onClearHistory = [this]() {
            std::string err;
            if (!printerHistory->clear(err))
                pom2::log().warn("PrinterHistory", err);
        };
    }
#ifdef __EMSCRIPTEN__
    host.canSaveFiles = false;   // MEMFS writes vanish on reload
#endif

    imageWriterPanel->render(&uiState_->showImageWriterPanel, *imageWriter, host);
}

void MainWindow::renderAiControlPanelWindow()
{
    if (!uiState_->showAiControlPanel) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Control (HTTP)", &uiState_->showAiControlPanel)) {
        ImGui::End();
        return;
    }

    const bool running = aiServer->isRunning();
    ImGui::Text("Status: %s", running ? "listening" : "stopped");
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("on 127.0.0.1:%u", aiServer->getPort());
    }
    ImGui::Text("Requests served: %llu",
                static_cast<unsigned long long>(aiServer->requestsServed()));
    {
        const std::string addr = aiServer->lastClientAddr();
        if (!addr.empty()) ImGui::Text("Last client: %s", addr.c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("TCP port", &aiPortInput, 0, 0);
    if (aiPortInput < 1)     aiPortInput = 1;
    if (aiPortInput > 65535) aiPortInput = 65535;

    char tokenBuf[128];
    std::snprintf(tokenBuf, sizeof(tokenBuf), "%s", aiTokenInput.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Auth token (empty = open)", tokenBuf, sizeof(tokenBuf))) {
        aiTokenInput = tokenBuf;
        aiServer->setAuthToken(aiTokenInput);
    }

    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button("Start")) {
            // Re-open slot endpoints; targets are resolved from SlotBus for
            // each request and are never cached by the server.
            aiServer->attach(controller.get(), display.get());
            aiServer->setAuthToken(aiTokenInput);
            if (!aiServer->start(static_cast<uint16_t>(aiPortInput))) {
                uiState_->tapeStatusMessage = "AI Control: bind failed (port busy?)";
                uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
            }
        }
    } else {
        if (ImGui::Button("Stop")) aiServer->stop();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Drive POM2 from an AI agent (or curl/Postman) via HTTP/1.1.");
    ImGui::Spacing();
    ImGui::TextDisabled("Example:");
    ImGui::TextWrapped("  curl http://127.0.0.1:%d/status", aiPortInput);
    ImGui::TextWrapped(
        "  curl -d '{\"text\":\"PRINT 1+1\\r\"}' http://127.0.0.1:%d/keyboard",
        aiPortInput);
    ImGui::TextWrapped(
        "  curl http://127.0.0.1:%d/screen.ppm -o screen.ppm",
        aiPortInput);
    if (!aiTokenInput.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Send 'X-POM2-Token: <token>' header on each request.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Endpoints: /status /reset /cpu /mem /keyboard /disk "
                        "/eject /snapshot/save /snapshot/load /speed /screen.ppm");

    ImGui::End();
}

// ─── Abstraction Levels (LLE / HLE) ──────────────────────────────────────
//
// The window is `AbstractionLevels_ImGui` and the catalog of subsystems is
// static data beside it; what lives here is the part only MainWindow can
// answer — which cards are on the bus, and which of them are running their
// real ROM versus a fallback. That distinction is the panel's reason to
// exist: `docs/lle_vs_hle.md` § "Keeping a level once you have it" names
// silent degradation as a structural hole, because every ROM-driven low
// level in POM2 falls back to a working higher one when its dump is absent
// and nothing anywhere says so.

bool MainWindow::swapSlotCardVariant(const char* fromKey, const char* toKey)
{
    // In place, in the slot the card already occupies: the two keys are the
    // same peripheral at two abstraction levels, so moving it would be a
    // second, unasked-for change (and would break any software that has the
    // slot number baked in, which for a mouse or a printer is most of it).
    pom2::SlotConfigurationCoordinator::LiveSnapshot liveSlots;
    {
        auto state = controller->lockState();
        liveSlots = slotCoordinator_->captureLive(state.memory().slotBus());
    }
    int slot = -1;
    for (int s = 1; s <= 7; ++s)
        if (liveSlots.keys[s] == fromKey) { slot = s; break; }
    if (slot < 0) return false;

    const std::string key      = "slot_" + std::to_string(slot) + "_card";
    const std::string previous = settings->getString(key, "");
    settings->setString(key, toKey);
    if (!settings->save()) {
        settings->setString(key, previous);
        uiState_->tapeStatusMessage = "Could not save the slot change.";
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 6.0;
        return false;
    }
    if (!restartEmulationFromSettings()) {
        // Rebuild refused — the live machine was deliberately left intact, so
        // the persisted key has to go back too or the refused change would
        // apply silently on the next launch. Same contract as Slot Config's
        // Apply.
        settings->setString(key, previous);
        settings->save();
        return false;
    }
    settings->save();
    pom2::log().info("Abstraction",
                     "slot " + std::to_string(slot) + ": " + fromKey +
                     " -> " + toKey);
    return true;
}

void MainWindow::renderAbstractionPanel()
{
    if (!uiState_->showAbstractionPanel) return;
    if (!abstractionPanel)
        abstractionPanel = std::make_unique<pom2::AbstractionLevels_ImGui>();

    using Panel = pom2::AbstractionLevels_ImGui;
    using Live  = Panel::Live;
    Panel::Snapshot snap;
    const auto deviceInventory = devicePanelCoordinator_->captureInventory();
    const auto mouseInventory = mouseCoordinator_->capture();
    pom2::SlotConfigurationCoordinator::LiveSnapshot liveSlots;
    {
        auto state = controller->lockState();
        liveSlots = slotCoordinator_->captureLive(state.memory().slotBus());
    }

    // Plug state comes from an immutable SlotBus snapshot, not from the
    // effective settings plan and not from retained card aliases.
    auto plugged = [&](const char* key) {
        for (int s = 1; s <= 7; ++s)
            if (liveSlots.keys[s] == key) return true;
        return false;
    };
    auto row = [&](const char* id, Live live, const char* detail = "") {
        Panel::Row r;
        r.id     = id;
        r.live   = live;
        r.detail = detail;
        snap.rows.push_back(std::move(r));
    };
    // A card that is plugged but running its fallback ROM: still working,
    // still wrong about its level. `actual` is where it really sits.
    auto degradable = [&](const char* id, bool isPlugged, bool atFullLevel,
                          pom2::AbsLevel fallback, const char* why) {
        Panel::Row r;
        r.id = id;
        if      (!isPlugged)   r.live = Live::NotPlugged;
        else if (atFullLevel)  r.live = Live::Active;
        else                 { r.live = Live::Degraded; r.actual = fallback;
                               r.detail = why; }
        snap.rows.push_back(std::move(r));
    };

    // ── Storage ─────────────────────────────────────────────────────────
    const auto storage = storageCoordinator_->captureInventory(*controller);
    // Disk II is the sharpest case in the whole table: no P6 dump and no WOZ
    // mounted means the legacy 32-cycle nibble gate, which reads stock DOS
    // 3.3 fine and loses every bitstream-reading protection. `usingBitLss()`
    // is the honest test — a mounted WOZ forces the L0 path even with no
    // roms/diskii_p6.rom, using the embedded default P6.
    degradable("diskii", storage.hasDiskII(),
               storage.primaryDiskUsingBitLss, pom2::AbsLevel::H1,
               "roms/diskii_p6.rom absent and no WOZ mounted — running the "
               "legacy 32-cycle nibble gate, which cannot decode "
               "bitstream-level copy protection.");
    row("diskimage", storage.hasDiskII() ? Live::Active : Live::NotPlugged);
    row("cffa", plugged("cffa")        ? Live::Active : Live::NotPlugged);
    row("hdv",  plugged("hdv")         ? Live::Active : Live::NotPlugged);
    degradable("smartportcard", plugged("smartport35"),
               storage.primarySmartPortLironRomLoaded,
               pom2::AbsLevel::H1,
               "roms/liron.rom absent — the slot page and the $C800 bank are "
               "POM2's synthetic firmware instead of the real Liron ROM.");
    {
        const bool iicClass =
            activeProfile == pom2::SystemProfile::AppleIIc ||
            activeProfile == pom2::SystemProfile::AppleIIcPlus ||
            activeProfile == pom2::SystemProfile::AppleIIcPAL;
        row("iicsp", iicClass ? Live::Active : Live::NotPlugged,
            iicClass ? "Armed only by an explicit boot from slot 5; every "
                       "reset disarms it." : "");
        row("iwm", activeProfile == pom2::SystemProfile::AppleIIcPlus
                       ? Live::Active : Live::NotPlugged);
        row("sony35", (iicClass || plugged("smartport35"))
                       ? Live::Active : Live::NotPlugged);
    }
    row("prodosvol", Live::NotApplicable);

    // ── Input, clocks, printing ─────────────────────────────────────────
    row("mouse", mouseInventory.mamePlugged
                     ? Live::Active : Live::NotPlugged);
    row("mouseaw", mouseInventory.appleWinPlugged
                       ? Live::Active : Live::NotPlugged);
    degradable("clock", deviceInventory.clockPlugged(),
               deviceInventory.clockRomFromDump, pom2::AbsLevel::H1,
               "roms/thunderclock_u9_v1.3.bin absent — running the synthetic "
               "ProDOS-signature stub, so tools that pull the driver off the "
               "card find nothing.");
    degradable("grappler", plugged("grappler"),
               deviceInventory.grapplerPlugged() &&
                   deviceInventory.grapplerRomLoaded,
               pom2::AbsLevel::H1,
               "roms/grappler_plus.bin absent — running buildStubRom(), so "
               "the real Orange Micro firmware is not executing.");
    row("printercard", plugged("printer") ? Live::Active : Live::NotPlugged);
    row("nsclock", controller->noSlotClock().isEnabled()
                       ? Live::Active : Live::NotPlugged);

    // ── Video ───────────────────────────────────────────────────────────
    // Exactly one of the two colour paths is running, which makes this pair
    // the clearest live illustration of the axis in the whole panel.
    {
        const auto hi = display->getHiResMode();
        const bool oe = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                         hi == Apple2Display::HiResMode::ColorCompositeOECpu);
        row("oe",  oe ? Live::Active : Live::NotPlugged,
            oe ? "Signal-level demodulation of the 14.318 MHz 1-bit stream."
               : "Pick a Composite (OpenEmulator) mode in Display to run it.");
        row("lut", oe ? Live::NotPlugged : Live::Active,
            oe ? "" : "Artifact colours are read from a table; no signal is "
                      "synthesised.");
    }
    row("chatmauve", plugged("chatmauve") ? Live::Active : Live::NotPlugged);

    // ── Audio, network, CPU ─────────────────────────────────────────────
    row("mockingboard", (plugged("mockingboard") || plugged("mockingboard_c") ||
                         plugged("phasor")) ? Live::Active : Live::NotPlugged);
    row("ssi263", (plugged("echoplus") || plugged("mockingboard_c"))
                      ? Live::Active : Live::NotPlugged);
    row("tms5220", plugged("echoplus_tms") ? Live::Active : Live::NotPlugged);
    row("ssc",       plugged("ssc")       ? Live::Active : Live::NotPlugged);
    row("uthernet",  plugged("uthernet")  ? Live::Active : Live::NotPlugged);
    row("uthernet2", plugged("uthernet2") ? Live::Active : Live::NotPlugged);
    row("fujinet",   plugged("fujinet")   ? Live::Active : Live::NotPlugged);
    row("softcard",  plugged("softcard")  ? Live::Active : Live::NotPlugged);
    row("z80",       plugged("softcard")  ? Live::Active : Live::NotPlugged);

    // ── The switchable boundaries ───────────────────────────────────────
    // Availability is gated on the dump each low side needs, because the
    // whole point of the panel is that a missing dump silently costs you a
    // level — offering a switch that would land on the fallback would repeat
    // the exact mistake it exists to expose.
    auto have = [](const char* rel) {
        return !pom2::findResource(rel).empty();
    };
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::MouseCard;
        t.title        = "Mouse Card";
        t.needsRestart = true;
        t.low.label    = "MAME — the M68705 MCU executes its mask ROM";
        t.low.level    = pom2::AbsLevel::L0;
        t.low.available = have("roms/mouse_341-0270-c.bin") &&
                          have("roms/mouse_341-0269.bin");
        t.low.blockedBy = "both roms/mouse_341-0270-c.bin (slot EPROM) and "
                          "roms/mouse_341-0269.bin (MCU mask ROM) are needed";
        t.low.why      = "Decodes real quadrature edges: at most one edge per\n"
                         "axis per MCU PortB read, so fast host motion is\n"
                         "rate-limited exactly as the hardware limits it.";
        t.high.label   = "AppleWin HLE — the MCU is a C++ state machine";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.available = have("roms/mouse_341-0270-c.bin");
        t.high.blockedBy = "roms/mouse_341-0270-c.bin (slot EPROM) is needed";
        t.high.why     = "Copies the host delta straight into the HLE'd MCU,\n"
                         "so it never drops motion — smoother, and less\n"
                         "correct. Needs a compensating absolute cursor sync\n"
                         "the L0 card does not.";
        t.selected = mouseInventory.mamePlugged
            ? 0 : (mouseInventory.appleWinPlugged ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add a mouse in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::BlockStorage;
        t.title        = "ProDOS block storage";
        t.needsRestart = true;
        t.low.label    = "CFFA 2.0 — the real 4 KB firmware executes over ATA";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/cffa20ee02.bin") ||
                          have("roms/cffa20eec02.bin");
        t.low.blockedBy = "roms/cffa20ee02.bin (or the 65C02 variant) is needed";
        t.low.why      = "An ATA taskfile model isomorphic to MAME's\n"
                         "cs0_r/cs0_w, driven by the card's own firmware.\n"
                         "Skips DMA / IRQ / SMART.";
        t.high.label   = "HDV card — synthetic ROM, 4-register port, memcpy";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "H1 IS the feature here: it mounts .hdv/.2mg\n"
                         "directly with no card ROM dump at all. The ProDOS\n"
                         "block corpus has no protection to lose.";
        t.selected     = plugged("cffa") ? 0 : (plugged("hdv") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::PrinterIface;
        t.title        = "Printer interface";
        t.needsRestart = true;
        t.low.label    = "Grappler+ — the real Orange Micro EPROM executes";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/grappler_plus.bin");
        t.low.blockedBy = "roms/grappler_plus.bin is needed";
        t.low.why      = "Status byte, register decode, $C800 banking and the\n"
                         "S1 DIPs, line-cited against MAME grappler.cpp.\n"
                         "What AppleWorks and the graphics dumps expect.";
        t.high.label   = "Printer card — synthetic ROM, PR#n hook only";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "A CSWL/CSWH hook and a 4-byte trampoline. No PROM\n"
                         "dump exists to run, and the Pascal entry block is\n"
                         "deliberately absent, so BASIC PR#n only.";
        t.selected     = plugged("grappler") ? 0 : (plugged("printer") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id         = pom2::AbsToggle::CompositeVideo;
        t.title      = "Colour pipeline";
        t.low.label  = "Composite (OpenEmulator) — demodulate a real signal";
        t.low.level  = pom2::AbsLevel::L1;
        t.low.why    = "The display emits a 14.318 MHz 1-bit luminance\n"
                       "stream; the shader demodulates Y/I/Q off the\n"
                       "subcarrier. Artifact colour is EMERGENT.";
        t.high.label = "Artifact LUT — look the colour up per dot pattern";
        t.high.level = pom2::AbsLevel::H1;
        t.high.why   = "MAME's composite colour tables: the RESULT of NTSC\n"
                       "artifacting, tabulated. Cheap, sharp, and unable to\n"
                       "show anything the table has no entry for.";
        const auto hi = display->getHiResMode();
        t.selected   = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                        hi == Apple2Display::HiResMode::ColorCompositeOECpu)
                           ? 0 : 1;
        t.note       = "Mono modes are neither — they bypass colour entirely.";
        snap.toggles.push_back(std::move(t));
    }

    const Panel::Request req = abstractionPanel->render(&uiState_->showAbstractionPanel,
                                                       snap);
    switch (req.toggle) {
        case pom2::AbsToggle::None:
            break;
        case pom2::AbsToggle::MouseCard:
            swapSlotCardVariant(req.option == 0 ? "mouseaw" : "mouse",
                                req.option == 0 ? "mouse" : "mouseaw");
            break;
        case pom2::AbsToggle::BlockStorage:
            swapSlotCardVariant(req.option == 0 ? "hdv" : "cffa",
                                req.option == 0 ? "cffa" : "hdv");
            break;
        case pom2::AbsToggle::PrinterIface:
            swapSlotCardVariant(req.option == 0 ? "printer" : "grappler",
                                req.option == 0 ? "grappler" : "printer");
            break;
        case pom2::AbsToggle::CompositeVideo:
            // No restart: the colour pipeline is a render-path choice, and
            // the machine never sees it. Persisted by the dtor's hi_res_mode
            // write, exactly like a View-menu pick.
            display->setHiResMode(
                req.option == 0 ? Apple2Display::HiResMode::ColorCompositeOE
                                : Apple2Display::HiResMode::ColorNTSC);
            lastColorHiResMode_ = display->getHiResMode();
            break;
    }
}

void MainWindow::renderNoSlotClockPanelWindow()
{
    if (!uiState_->showNoSlotClockPanel) return;

    ImGui::SetNextWindowSize(ImVec2(420, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("No-Slot Clock (Dallas DS1216E)###nsclockPanel",
                      &uiState_->showNoSlotClockPanel)) {
        ImGui::End();
        return;
    }

    pom2::NoSlotClock& nsc = controller->noSlotClock();
    bool enabled = nsc.isEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        nsc.setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Dallas DS1216E SmartWatch — virtual chip under the\n"
            "internal ROM. AppleWin-parity placement:\n"
            "  II / II+   : under Monitor ROM at $F800-$FFFF\n"
            "  //e / //c  : under $C300 + $C800 internal ROM\n"
            "ProDOS 2.0.3+ / GS-OS walk the 64-bit magic key\n"
            "0x5CA33AC55CA33AC5 (A2=0 reads, A0 = next bit),\n"
            "then read 64 clock bits via A2=1 reads on D0.");
    }

    ImGui::Separator();
    const auto phase = nsc.phase();
    const char* phaseName = (phase == pom2::NoSlotClock::Phase::Idle)
        ? "idle (pass-through)"
        : (phase == pom2::NoSlotClock::Phase::MatchingKey)
            ? "matching magic key"
            : "reading clock register";
    ImGui::Text("Phase: %s", phaseName);
    ImGui::Text("Key bits matched : %d / 64", nsc.keyBitsMatched());
    ImGui::Text("Clock bits read  : %d / 64", nsc.clockBitsRead());

    ImGui::Separator();
    ImGui::TextWrapped(
        "Place a free clock card in a slot for older software, or "
        "leave this enabled for ProDOS 2.0.3+/GS-OS auto-detection "
        "on any profile (incl. //c, where no slot card can exist).");

    ImGui::End();
}

// ─── 3D voxel view settings (MicroM8 "Voxel Cube") ───────────────────────
//
// Live sliders for the geometry knobs the Voxel3DRenderer exposes: cube
// thickness, the per-colour forward "pop", footprint fill, supersample
// (anti-alias) factor and the ambient floor. The grid resolution is NOT a
// knob — it always tracks the live display (one voxel per Apple II pixel).
// Values persist under the `voxel_*` keys; they bind straight to `voxel3d_`
// (owned up-front at settings-load, so the panel works before the view is on).
void MainWindow::renderVoxelSettingsWindow()
{
    if (!uiState_->showVoxelSettings) return;
    if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();

    ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("3D Voxel View", &uiState_->showVoxelSettings)) {
        ImGui::End();
        return;
    }

    // Quick enable toggle, mirroring the View-menu item so the panel is usable
    // stand-alone. Greys out the knobs while the 3D view is off.
    ImGui::Checkbox("Enable 3D voxel view", &uiState_->show3dVoxel);
    ImGui::SameLine();
    ImGui::TextDisabled("(left-drag orbit · middle-drag pan · wheel zoom)");
    ImGui::Separator();

    ImGui::BeginDisabled(!uiState_->show3dVoxel);

    pom2::Voxel3DRenderer& v = *voxel3d_;
    ImGui::SliderFloat("Voxel depth",  &v.voxelDepth, 0.0f, 12.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform Z-thickness of every cube (in pixel units).");
    ImGui::SliderFloat("Colour pop",   &v.colorShift, 0.0f, 24.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Z-axis 3D offset': brighter pixels push\n"
                          "toward the viewer for pin-art relief. 0 = flat slab.");
    ImGui::SliderFloat("Cube fill",    &v.cubeFill,   0.2f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Footprint as a fraction of the cell. 1.0 = contiguous\n"
                          "(no gap grid — best against moiré); lower = visible gaps.");
    ImGui::SliderInt  ("Anti-alias",   &v.superSample, 1, 4, "%dx supersample", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FBO render scale, minify-downsampled. Higher = smoother\n"
                          "edges (kills moiré) but more GPU. 1 = off.");
    ImGui::SliderFloat("Ambient",      &v.ambient,    0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lighting floor so no cube face goes pure black.");

    ImGui::Checkbox("Mono", &v.mono);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Voxel Cube Mono' — grey output, relief kept.");
    ImGui::SameLine();
    ImGui::Checkbox("Per-colour depth", &v.perColorDepth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap each pixel to the nearest lo-res palette colour and\n"
                          "drive 'Colour pop' from that — discrete, blocky relief\n"
                          "(MicroM8 per-index Z) instead of the smooth luminance field.");

    ImGui::Separator();
    if (ImGui::Button("Reset view")) {
        voxelCam_ = pom2::OrbitCamera{};
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset settings")) {
        pom2::Voxel3DRenderer def;
        v.voxelDepth    = def.voxelDepth;
        v.colorShift    = def.colorShift;
        v.cubeFill      = def.cubeFill;
        v.superSample   = def.superSample;
        v.ambient       = def.ambient;
        v.mono          = def.mono;
        v.perColorDepth = def.perColorDepth;
    }

    ImGui::EndDisabled();
    ImGui::End();
}

// ─── CRT Settings (Composite NTSC mode) ──────────────────────────────────
//
// Sliders that drive the OpenEmulator-style shader: standard four TV knobs
// (B/C/S/H), sharpness (chroma bandwidth), persistence (CRT afterglow), and
// the pure post-effects (scanlines, barrel, vignette, shadow mask). All
// values are persisted to settings.json under the `ntsc_*` keys so the look
// survives across sessions. No look presets: they overwrote the whole glass
// block in one click, which made the panel hard to reason about — the
// defaults plus "Reset to defaults" are the only starting points now.
void MainWindow::renderNtscSettingsWindow()
{
    if (!uiState_->showNtscSettings) return;

    ImGui::SetNextWindowSize(ImVec2(380, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CRT Settings (Composite NTSC)",
                      &uiState_->showNtscSettings)) {
        ImGui::End();
        return;
    }

    const pom2::Palette& pal = pom2::palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    // Master ON/OFF for every CRT effect, full-width at the top of the window.
    // Off bypasses the whole effect stack (the colour pipeline still runs);
    // the controls below grey out so it's clear they have no effect.
    {
        const bool on = crtEffectsEnabled;
        const ImVec4 col = u32(on ? pal.ok : pal.danger);
        // Tint the face at low alpha and put the full-strength colour on the
        // text: a saturated full-width slab was the loudest thing in the UI.
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(col.x, col.y, col.z, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(col.x, col.y, col.z, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(col.x, col.y, col.z, 0.44f));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::Button(on ? "CRT effects: ON  —  click to disable"
                             : "CRT effects: OFF  —  click to enable",
                          ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            crtEffectsEnabled = !crtEffectsEnabled;
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::BeginDisabled(!crtEffectsEnabled);

    pom2::NtscParams p = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
    bool changed = false;

    // ── Scope notes ──────────────────────────────────────────────────────
    // What actually applies right now. Kept terse and dim: it is reference
    // material, not a warning.
    const bool oeFamily =
        display->getHiResMode() == Apple2Display::HiResMode::ColorCompositeOE ||
        display->getHiResMode() == Apple2Display::HiResMode::ColorCompositeOECpu;
    if (!oeFamily) {
        ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
        ImGui::TextWrapped(
            "Every glass control below applies on this mode. PAL composite and "
            "Sharp text are demodulation-only — they affect just the two "
            "'Composite (OpenEmulator)' pipelines.");
        ImGui::PopStyleColor();
    }

    if (ntscFx && !ntscFx->available()) {
        // Previously this read as a flat contradiction: a green "CRT Effects:
        // ON" banner immediately above a red "Shader unavailable", leaving the
        // user unable to tell whether the controls below did anything. Scope
        // it explicitly — only the OpenEmulator *demodulation* shader is
        // missing; the CRT glass stack is a separate pass and still runs.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.warn));
        ImGui::TextWrapped(
            "OpenEmulator demodulation shader unavailable — the two "
            "'Composite (OpenEmulator)' pipelines fall back to the NTSC LUT. "
            "The glass controls below are a separate pass and still apply.");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ntscFx->lastError().c_str());
    }

    // ── Advanced ─────────────────────────────────────────────────────────
    // Open by default: the look presets that used to be the panel's primary
    // control are gone, so the sliders are the only controls left and hiding
    // them behind a collapsed header would leave the panel empty on open.
    // Labels lead, sliders fill the rest of the row:
    // ImGui's native SliderFloat puts its label on the RIGHT, which made the
    // panel read "bar → number → name" and clipped the longest label
    // ("Phosphor curve (ga…"). Two decimals, not three — these are perceptual
    // knobs and 0.055 was false precision.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Widest label sets the column, so it can never clip. Measured rather
        // than hardcoded so it survives the UI zoom.
        const float labelW = ImGui::CalcTextSize("Phosphor gamma").x +
                             ImGui::GetStyle().ItemSpacing.x * 2.0f;
        auto slider = [&](const char* label, const char* id, float* v,
                          float lo, float hi, const char* tip) {
            ImGui::TextUnformatted(label);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(id, v, lo, hi, "%.2f",
                                   ImGuiSliderFlags_AlwaysClamp))
                changed = true;
        };

        ImGui::SeparatorText("Picture");
        slider("Brightness", "##bright", &p.brightness, -0.5f, 0.5f,
               "Added to luma.");
        slider("Contrast",   "##contrast", &p.contrast,  0.5f, 1.5f,
               "Scaling around mid-grey.");
        slider("Saturation", "##sat", &p.saturation, 0.0f, 2.0f,
               "Chroma multiplier. 0 = monochrome.");
        slider("Hue",        "##hue", &p.hue, -0.5f, 0.5f,
               "I/Q rotation. Full turn at +/-0.5.");

        ImGui::SeparatorText("Phosphor");
        slider("Sharpness",   "##sharp", &p.sharpness, 0.0f, 1.0f,
               "Chroma bandwidth. Lower = more composite bleed.");
        slider("Persistence", "##persist", &p.persistence, 0.0f, 0.95f,
               "Temporal decay — the phosphor's afterglow.");
        slider("Phosphor gamma", "##gamma", &p.phosphorGamma, 0.6f, 2.6f,
               "Response curve. 1.0 = flat, >1 deepens shadows.\n"
               "Pairs with Persistence as the phosphor model.");

        ImGui::SeparatorText("Glass");
        slider("Scanlines", "##scan", &p.scanlines, 0.0f, 1.0f,
               "0 = off, 1 = black between every line.");
        slider("Barrel",    "##barrel", &p.barrel, 0.0f, 0.30f,
               "Tube curvature. 0 = flat.");
        slider("Vignette",  "##vign", &p.centerLighting, 0.5f, 1.0f,
               "Center lighting. 1.0 = flat, lower darkens the edges.");

        // Shadow mask: combo + strength. Procedural — no texture upload, no
        // perf cost when Off.
        static const char* kMaskNames[] = {
            "Off", "Triad (3-stripe)", "Aperture grille (Trinitron)",
            "Dot mask (offset triads)"
        };
        int maskIdx = static_cast<int>(p.shadowMask);
        ImGui::TextUnformatted("Shadow mask");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##mask", &maskIdx, kMaskNames,
                         IM_ARRAYSIZE(kMaskNames))) {
            p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(maskIdx);
            changed = true;
        }
        ImGui::BeginDisabled(p.shadowMask == pom2::NtscParams::ShadowMask::Off);
        slider("Mask strength", "##maskstr", &p.shadowMaskStrength, 0.0f, 1.0f,
               nullptr);
        ImGui::EndDisabled();
        slider("Luminance gain", "##lumgain", &p.luminanceGain, 1.0f, 2.0f,
               "Post-glass re-brighten — compensates the dimming\n"
               "from scanlines and the shadow mask.");

        ImGui::SeparatorText("Demodulation (OpenEmulator pipelines only)");
        // PAL composite — line-phase alternation. Off by default (POM2 ships
        // with the NTSC look most users associate with the Apple II). It
        // describes the machine being emulated, not a look.
        changed |= ImGui::Checkbox("PAL composite (line-phase alternation)",
                                   &p.palMode);
        // Sharp-text bypass: keep glyphs crisp in TEXT mode by skipping the
        // shader for the whole text screen. HGR/DHGR/lo-res still run through
        // the demodulator.
        changed |= ImGui::Checkbox("Sharp text (bypass shader in TEXT mode)",
                                   &p.textSharp);

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults")) {
            p = pom2::NtscParams{};
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to ntsc_* keys");
    }

    ImGui::EndDisabled();

    if (changed) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        ntscFx->setParams(p);
    }

    ImGui::End();
}

void MainWindow::renderJoystickPanelWindow()
{
    if (!uiState_->showJoystickPanel) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick->deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick->binding();
    snap.hostIdx    = cf.hostIdx;
    snap.deadzone   = cf.deadzone;
    snap.invert     = cf.invert;
    snap.squareGate = cf.squareGate;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick->paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick->buttonDown(i);

    auto result = joystickPanel->render("Joystick", uiState_->showJoystickPanel, snap);
    if (result.changed) {
        auto& bind = joystick->binding();
        bind.hostIdx    = result.hostIdx;
        bind.deadzone   = result.deadzone;
        bind.invert     = result.invert;
        bind.squareGate = result.squareGate;
        if (settings) settings->setBool("joystick_square_gate", bind.squareGate);
    }
}

// ─── Mouse Inspector ─────────────────────────────────────────────────────
//
// Diagnostic panel for tuning Apple II Mouse Card alignment. Live readout
// of: host cursor (window coords + widget-local + in-widget fraction),
// Apple II Screen widget rect + per-axis logical→host scale, MouseCard's
// 8-bit running counter + sub-pixel accumulator, AppleWin HLE firmware
// state (clamp window, current iX/iY, MOUSE_READ snapshot, mode/state
// bits, PIA port latches, last command), and the AppleMouse firmware
// screen holes for the active slot. Optional CSV log at ~30 Hz so a
// session of cursor motion can be replayed offline.

void MainWindow::renderMouseInspectorWindow()
{
    if (!uiState_->showMouseInspector) return;
    ImGui::SetNextWindowPos (ImVec2(40, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mouse Inspector", &uiState_->showMouseInspector)) {
        ImGui::End();
        return;
    }

    const auto mouse = mouseCoordinator_->capture();

    const float widgetW = uiState_->screenRectMax.x - uiState_->screenRectMin.x;
    const float widgetH = uiState_->screenRectMax.y - uiState_->screenRectMin.y;
    const double hostLocalX = uiState_->lastMouseHostX - double(uiState_->screenRectMin.x);
    const double hostLocalY = uiState_->lastMouseHostY - double(uiState_->screenRectMin.y);
    const bool hostInside =
        widgetW > 0.0f && widgetH > 0.0f &&
        uiState_->lastMouseHostX >= double(uiState_->screenRectMin.x) &&
        uiState_->lastMouseHostX <= double(uiState_->screenRectMax.x) &&
        uiState_->lastMouseHostY >= double(uiState_->screenRectMin.y) &&
        uiState_->lastMouseHostY <= double(uiState_->screenRectMax.y);
    const double fracX = widgetW > 0.0f ? hostLocalX / double(widgetW) : 0.0;
    const double fracY = widgetH > 0.0f ? hostLocalY / double(widgetH) : 0.0;
    const int dispW = display->width();
    const int dispH = display->height();
    // Apple-cursor pixels per host pixel — what onMouseMove uses to scale
    // host deltas to MCU 8-bit counts. Always derived from the constant
    // kWidth/kHeight (the widget is rendered at that aspect, not at the
    // current display resolution — see the comment in onMouseMove).
    const double sxRatio =
        widgetW > 0.0f ? double(Apple2Display::kWidth)  / double(widgetW) : 0.0;
    const double syRatio =
        widgetH > 0.0f ? double(Apple2Display::kHeight) / double(widgetH) : 0.0;

    // ── Host cursor ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Host cursor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Window coords : (%.1f, %.1f)", uiState_->lastMouseHostX, uiState_->lastMouseHostY);
        ImGui::Text("Widget-local  : (%.1f, %.1f)", hostLocalX, hostLocalY);
        ImGui::Text("Fraction      : (%.3f, %.3f)", fracX, fracY);
        ImGui::Text("Button held   : %s", uiState_->mouseButtonHeld ? "YES" : "no");
        ImGui::TextColored(
            hostInside ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                       : ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            "Inside Apple II Screen widget: %s", hostInside ? "YES" : "no");
        // Captured, "Window coords" above is GLFW's *virtual* unbounded
        // position — it walks past the window edges and the widget-local /
        // fraction rows below it stop meaning anything. Say so rather than
        // letting the numbers read as a bug.
        ImGui::TextColored(
            uiState_->mouseGrabbed ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                          : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Pointer captured (grab)     : %s",
            uiState_->mouseGrabbed ? "YES — coords above are virtual" : "no");
    }

    // ── Apple II Screen widget rect ───────────────────────────────────
    if (ImGui::CollapsingHeader("Apple II Screen widget",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Rect min      : (%.1f, %.1f)", uiState_->screenRectMin.x, uiState_->screenRectMin.y);
        ImGui::Text("Rect max      : (%.1f, %.1f)", uiState_->screenRectMax.x, uiState_->screenRectMax.y);
        ImGui::Text("Size          : %.1f x %.1f host px", widgetW, widgetH);
        ImGui::Text("Display res   : %d x %d (kWidth=%d kHeight=%d)",
                    dispW, dispH, Apple2Display::kWidth, Apple2Display::kHeight);
        ImGui::Text("Apple px/host : %.4f x %.4f (used by onMouseMove)",
                    sxRatio, syRatio);
    }

    // ── MouseCard 8-bit running counter (MainWindow side) ─────────────
    if (ImGui::CollapsingHeader("MouseCard input (8-bit counter)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Apple counter : (%3u, %3u)  [0x%02X, 0x%02X]",
                    uiState_->mouseAppleX, uiState_->mouseAppleY, uiState_->mouseAppleX, uiState_->mouseAppleY);
        ImGui::Text("Sub-pixel acc : (%.3f, %.3f)",
                    uiState_->mouseSubAppleX, uiState_->mouseSubAppleY);
        const char* cardName =
            mouse.appleWinActive() ? "AppleWin HLE (mouseaw)" :
            mouse.kind == pom2::MouseCoordinator::Kind::Mame
                ? "MAME-faithful (mouse)" : "(no card plugged)";
        ImGui::Text("Active card   : %s", cardName);
    }

    // ── AppleWin HLE card-internal state ──────────────────────────────
    if (mouse.appleWinActive()) {
        if (ImGui::CollapsingHeader("AppleWin HLE — firmware state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& s = mouse.appleWin;
            ImGui::Text("Clamp X       : [%d .. %d]", s.iMinX, s.iMaxX);
            ImGui::Text("Clamp Y       : [%d .. %d]", s.iMinY, s.iMaxY);
            ImGui::Text("Cursor iX/iY  : (%d, %d)", s.iX, s.iY);
            ImGui::Text("Read   nX/nY  : (%d, %d)  (last MOUSE_READ snap)",
                        s.nX, s.nY);
            ImGui::Text("Buttons curr  : btn0=%d btn1=%d   prev: btn0=%d btn1=%d",
                        s.bBtn0, s.bBtn1, s.bPrevBtn0, s.bPrevBtn1);
            ImGui::Text("MODE  ($00)   : 0x%02X  on=%d intMove=%d intBtn=%d intVBL=%d",
                        s.byMode,
                        (s.byMode & 0x01) ? 1 : 0,
                        (s.byMode & 0x02) ? 1 : 0,
                        (s.byMode & 0x04) ? 1 : 0,
                        (s.byMode & 0x08) ? 1 : 0);
            ImGui::Text("STATE byte    : 0x%02X  curBtn0=%d curBtn1=%d moved=%d",
                        s.byState,
                        (s.byState & 0x80) ? 1 : 0,
                        (s.byState & 0x10) ? 1 : 0,
                        (s.byState & 0x20) ? 1 : 0);
            const char* cmdName = "(unknown)";
            switch (s.lastCmd & 0xF0) {
                case 0x00: cmdName = "MOUSE_SET";   break;
                case 0x10: cmdName = "MOUSE_READ";  break;
                case 0x20: cmdName = "MOUSE_SERV";  break;
                case 0x30: cmdName = "MOUSE_CLEAR"; break;
                case 0x40: cmdName = "MOUSE_POS";   break;
                case 0x50: cmdName = "MOUSE_INIT";  break;
                case 0x60: cmdName = "MOUSE_CLAMP"; break;
                case 0x70: cmdName = "MOUSE_HOME";  break;
                case 0x90: cmdName = "MOUSE_TIME";  break;
            }
            ImGui::Text("Last cmd byte : 0x%02X (%s)  buffPos=%d dataLen=%d",
                        s.lastCmd, cmdName, s.buffPos, s.dataLen);
            ImGui::Text("PIA latches   : A=0x%02X  B=0x%02X", s.by6821A, s.by6821B);
        }
    } else if (mouse.kind == pom2::MouseCoordinator::Kind::Mame) {
        if (ImGui::CollapsingHeader("MAME-faithful — card state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled(
                "Firmware position lives inside the 68705P3 MCU RAM —");
            ImGui::TextDisabled(
                "use the screen-hole readout below for the cursor state.");
        }
    }

    // ── AppleMouse firmware screen holes (per Apple II Mouse FAQ) ─────
    if (ImGui::CollapsingHeader("Screen holes (AppleMouse firmware)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const int activeSlot = mouse.slot;
        if (activeSlot < 1 || activeSlot > 7) {
            ImGui::TextDisabled("(no mouse card plugged)");
        } else {
            const auto& holes = mouse.holes;
            const int holeXlo = holes.xLo;
            const int holeXhi = holes.xHi;
            const int holeYlo = holes.yLo;
            const int holeYhi = holes.yHi;
            const int holeMode = holes.mode;
            const int holeStatus = holes.status;
            const int holeX = holes.x();
            const int holeY = holes.y();
            ImGui::Text("Slot          : %d", activeSlot);
            ImGui::Text("X = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x0478 + activeSlot, holeX,
                        0x0478 + activeSlot, holeXlo,
                        0x0578 + activeSlot, holeXhi);
            ImGui::Text("Y = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x04F8 + activeSlot, holeY,
                        0x04F8 + activeSlot, holeYlo,
                        0x05F8 + activeSlot, holeYhi);
            ImGui::Text("Mode  $%04X   : 0x%02X (bit0=mouseOn=%d)",
                        0x07F8 + activeSlot, holeMode, holeMode & 0x01);
            ImGui::Text("Status $%04X  : 0x%02X (bit7=btnDown bit5=moved)",
                        0x0778 + activeSlot, holeStatus);
        }
    }

    // ── CSV logging ───────────────────────────────────────────────────
    ImGui::Separator();
    const bool logging = uiState_->mouseInspectorLogStream != nullptr;
    if (!logging) {
        if (ImGui::Button("Start logging to CSV")) {
            uiState_->mouseInspectorLogPath = "mouse_inspector.csv";
            uiState_->mouseInspectorLogStream =
                std::make_unique<std::ofstream>(uiState_->mouseInspectorLogPath);
            if (*uiState_->mouseInspectorLogStream) {
                *uiState_->mouseInspectorLogStream
                    << "t_s,hostX,hostY,inside,widgetMinX,widgetMinY,"
                       "widgetW,widgetH,appleCntX,appleCntY,btn,"
                       "awIX,awIY,awMinX,awMaxX,awMinY,awMaxY,"
                       "awMode,awState,holeX,holeY,holeMode\n";
                uiState_->mouseInspectorLastLogTime = 0.0;
                pom2::log().info("MouseInspector",
                    "Logging to " + uiState_->mouseInspectorLogPath);
            } else {
                uiState_->mouseInspectorLogStream.reset();
                pom2::log().warn("MouseInspector",
                    "Cannot open " + uiState_->mouseInspectorLogPath);
            }
        }
    } else {
        if (ImGui::Button("Stop logging")) {
            uiState_->mouseInspectorLogStream.reset();
            pom2::log().info("MouseInspector",
                "Stopped logging to " + uiState_->mouseInspectorLogPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", uiState_->mouseInspectorLogPath.c_str());
    }
    ImGui::TextDisabled(
        "CSV row per ~33 ms (panel-driven); flushed after each row.");

    // Rate-limit to ~30 Hz so a 5-minute capture stays small. Use
    // ImGui's frame time (monotonic, decoupled from emulated CPU
    // cycles) — the panel is paced by the UI loop, not the worker.
    if (uiState_->mouseInspectorLogStream) {
        const double now = ImGui::GetTime();
        if (now - uiState_->mouseInspectorLastLogTime >= 1.0 / 30.0) {
            uiState_->mouseInspectorLastLogTime = now;
            const int holeX = mouse.holes.x();
            const int holeY = mouse.holes.y();
            const int holeMode = mouse.holes.mode;
            const auto& s = mouse.appleWin;
            auto& os = *uiState_->mouseInspectorLogStream;
            os << now << ','
               << uiState_->lastMouseHostX << ',' << uiState_->lastMouseHostY << ','
               << (hostInside ? 1 : 0) << ','
               << uiState_->screenRectMin.x << ',' << uiState_->screenRectMin.y << ','
               << widgetW << ',' << widgetH << ','
               << int(uiState_->mouseAppleX) << ',' << int(uiState_->mouseAppleY) << ','
               << (uiState_->mouseButtonHeld ? 1 : 0) << ','
               << s.iX << ',' << s.iY << ','
               << s.iMinX << ',' << s.iMaxX << ','
               << s.iMinY << ',' << s.iMaxY << ','
               << int(s.byMode) << ',' << int(s.byState) << ','
               << holeX << ',' << holeY << ',' << holeMode << '\n';
            os.flush();
        }
    }

    ImGui::End();
}

// ─── Audio Mixer ─────────────────────────────────────────────────────────
//
// Consolidated mixer panel: one row per source (Master, Speaker, Cassette,
// Mockingboard if plugged, Disk 5.25", Disk 3.5" if its sample bank
// loaded). Sliders + mute checkboxes write directly into the underlying
// atomics on each source / on AudioDevice — no UI-side cache, so cross-
// thread read-back stays consistent. Replaces the two sliders that used
// to live in the Status panel.

void MainWindow::renderAudioMixerWindow()
{
    if (!uiState_->showAudioMixer) return;

    // The default size has to cover a full row — label column + volume
    // slider + meter + Mute + pan — or the rightmost control lands outside
    // the window with no way to reach it. Scale it, because everything the
    // row is built from (font, padding) scales with the UI zoom while a
    // literal pixel size would not.
    const float uiSc = uiScale_ * dpiScale_;
    ImGui::SetNextWindowPos (ImVec2(80, 80),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(540 * uiSc, 320 * uiSc), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Mixer", &uiState_->showAudioMixer)) {
        ImGui::End();
        return;
    }

    // Row geometry. The columns used to hang off hard-coded 110/160/40/90 px
    // offsets, which do NOT follow uiScale_/dpiScale_ while the text and
    // padding around them do: the pan slider already sat ~100 px past the
    // content edge at this panel's own default size, and every zoom step
    // made it worse. Derive the label column and the meter from the font
    // (which does scale) and let the two sliders share whatever width the
    // window actually offers, so the row stays whole at any scale and in
    // any docked width.
    const float labelColW = 8.0f * ImGui::GetFontSize();

    // `panSrc` is the AudioSource whose stereo placement this row edits,
    // or null for a row that has no placement to offer: the master bus
    // (it IS the stereo field) and the AY cards, which pan themselves
    // per chip from the card's wiring — see AudioDevice.h.
    auto channelRow = [labelColW](const char* label, float& vol, bool& mute,
                         float peak, const char* idSuffix, bool dim,
                         AudioSource* panSrc = nullptr) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float em     = ImGui::GetFontSize();
        const float avail  = ImGui::GetContentRegionAvail().x;
        // A label longer than the column (the "(samples missing)" rows)
        // pushes its own row right instead of being overdrawn by the slider.
        const float labelW = std::max(labelColW,
                                      ImGui::CalcTextSize(label).x + st.ItemSpacing.x);
        const float meterW = 3.0f * em;
        const float muteW  = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
                           + ImGui::CalcTextSize("Mute").x;
        const float fixedW = labelW + meterW + muteW
                           + (panSrc ? 4.0f : 3.0f) * st.ItemSpacing.x;
        // Floor at a still-draggable width: below that the user has shrunk
        // the panel past what the controls need and clipping is on them.
        const float freeW  = std::max(6.0f * em, avail - fixedW);
        const float panW   = panSrc ? std::min(6.5f * em, freeW * 0.40f) : 0.0f;
        const float volW   = freeW - panW;

        if (dim) ImGui::BeginDisabled();
        ImGui::PushID(idSuffix);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(volW);
        const std::string slid = std::string("##v") + idSuffix;
        ImGui::SliderFloat(slid.c_str(), &vol, 0.0f, 2.0f, "%.2f");
        // Tiny activity meter: lets the user confirm at a glance that
        // the channel is actually producing samples (addresses the
        // "doesn't seem connected to the mixer" feedback). Green for
        // safe levels, yellow approaching clip, red at clip.
        ImGui::SameLine();
        const float clamped = std::min(1.0f, std::max(0.0f, peak));
        ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
        if      (clamped >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
        else if (clamped >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(clamped, ImVec2(meterW, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::string muteId = std::string("Mute##") + idSuffix;
        ImGui::Checkbox(muteId.c_str(), &mute);
        // Stereo placement for a mono source. Centre (0) is unity on
        // both channels, so leaving it alone reproduces the pre-stereo
        // mix exactly.
        if (panSrc) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(panW);
            float pan = panSrc->pan.load(std::memory_order_relaxed);
            const std::string panId = std::string("##p") + idSuffix;
            // Build the whole readout ourselves: ImGui takes this string
            // as a printf FORMAT applied to the value, and one with no
            // conversion specifier is printed literally — which is how
            // the "centre" case works, and it saves showing a signed
            // number the user would have to decode. Keep it free of any
            // '%' for that reason: a literal one would have to be
            // doubled or snprintf reads past the end of the format.
            char fmt[24];
            if (pan < -0.005f)
                std::snprintf(fmt, sizeof fmt, "L %.2f", -pan);
            else if (pan > 0.005f)
                std::snprintf(fmt, sizeof fmt, "R %.2f",  pan);
            else
                std::snprintf(fmt, sizeof fmt, "centre");
            if (ImGui::SliderFloat(panId.c_str(), &pan, -1.0f, 1.0f, fmt))
                panSrc->pan.store(pan, std::memory_order_relaxed);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                panSrc->pan.store(0.0f, std::memory_order_relaxed);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stereo placement — right-click to centre");
        }
        ImGui::PopID();
        if (dim) ImGui::EndDisabled();
    };

    // ── Master ─────────────────────────────────────────────────────────
    AudioDevice& dev = controller->audio();
    float masterVol = dev.getMasterVolume();
    bool  masterMute = dev.isMasterMuted();
    channelRow("Master", masterVol, masterMute, dev.getMasterPeak(),
               "master", false);
    if (masterVol != dev.getMasterVolume()) dev.setMasterVolume(masterVol);
    if (masterMute != dev.isMasterMuted()) dev.setMasterMuted(masterMute);
    // The bus is stereo, so the master needs a meter per channel — the
    // single bar above shows the louder side, which cannot tell a
    // hard-panned card from a centred one.
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("L / R");
        ImGui::SameLine(labelColW);
        // Two bars spanning roughly the volume slider above them — same
        // font-relative unit, so they stay aligned under UI scaling.
        const float barW = 5.6f * ImGui::GetFontSize();
        const auto bar = [barW](float v) {
            const float c = std::min(1.0f, std::max(0.0f, v));
            ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
            if      (c >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
            else if (c >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            ImGui::ProgressBar(c, ImVec2(barW, 0.0f), "");
            ImGui::PopStyleColor();
        };
        bar(dev.getMasterPeakL());
        ImGui::SameLine();
        bar(dev.getMasterPeakR());
        ImGui::SameLine();
        bool mono = dev.isMonoDownmix();
        if (ImGui::Checkbox("Mono", &mono)) dev.setMonoDownmix(mono);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Fold the stereo bus down to a centred image.\n"
                "The Mockingboard pans AY1 left and AY2 right on real\n"
                "hardware, so a single-chip tune plays from one speaker;\n"
                "tick this for mono gear or a centred mix.");
        }
    }
    ImGui::Separator();

    // ── Speaker ────────────────────────────────────────────────────────
    SpeakerDevice& spk = controller->speaker();
    float spkVol = spk.getVolume();
    bool  spkMute = spk.isMuted();
    channelRow("Speaker", spkVol, spkMute,
               spk.lastBufferPeak.load(std::memory_order_relaxed),
               "spk", false, &spk);
    if (spkVol != spk.getVolume()) spk.setVolume(spkVol);
    if (spkMute != spk.isMuted()) spk.setMuted(spkMute);

    // ── Cassette ───────────────────────────────────────────────────────
    CassetteDevice& tape = controller->cassette();
    float tapeVol = tape.getVolume();
    bool  tapeMute = tape.isMuted();
    channelRow("Cassette", tapeVol, tapeMute,
               tape.lastBufferPeak.load(std::memory_order_relaxed),
               "tape", false, &tape);
    if (tapeVol != tape.getVolume()) tape.setVolume(tapeVol);
    if (tapeMute != tape.isMuted()) tape.setMuted(tapeMute);

    // ── Slot audio cards (Mockingboard / Phasor / Echo+) ──────────────
    // The snapshot enumerates every live card, so coexisting Mockingboard
    // variants each get a row. Commands re-resolve (kind, slot) under the
    // machine lock; no pointer survives this frame.
    for (const auto& card : audioCoordinator_->captureMixerCards()) {
        const char* typeLabel = "Audio";
        const char* typeId = "card";
        switch (card.kind) {
            case pom2::AudioCoordinator::CardKind::Mockingboard:
                typeLabel = "Mockingbd";
                typeId = "mb";
                break;
            case pom2::AudioCoordinator::CardKind::Phasor:
                typeLabel = "Phasor";
                typeId = "phasor";
                break;
            case pom2::AudioCoordinator::CardKind::EchoPlus:
                typeLabel = "Echo+";
                typeId = "echop";
                break;
            case pom2::AudioCoordinator::CardKind::EchoPlusTms5220:
                continue; // scaffold has no AudioSource yet
        }
        float volume = card.volume;
        bool muted = card.muted;
        const std::string label = std::string(typeLabel) + " (S" +
                                  std::to_string(card.slot) + ")";
        const std::string id = std::string(typeId) + "_s" +
                               std::to_string(card.slot);
        channelRow(label.c_str(), volume, muted, card.peak, id.c_str(), false);
        if (volume != card.volume || muted != card.muted) {
            pom2::AudioCoordinator::MixerCardCommand command;
            command.kind = card.kind;
            command.slot = card.slot;
            command.volume = volume;
            command.muted = muted;
            (void)audioCoordinator_->applyMixerCard(command);
        }
    }

    // ── Disk 5.25" ─────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs525 = controller->floppySound525();
        const bool dim = !fs525.isLoaded();
        float vol = fs525.getVolume();
        bool  mute = fs525.isMuted();
        channelRow(dim ? "Disk 5.25\" (samples missing)" : "Disk 5.25\"",
                   vol, mute,
                   fs525.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs525", dim, &fs525);
        if (!dim) {
            if (vol != fs525.getVolume()) fs525.setVolume(vol);
            if (mute != fs525.isMuted()) fs525.setMuted(mute);
        }
    }

    // ── Disk 3.5" ──────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs35 = controller->floppySound35();
        const bool dim = !fs35.isLoaded();
        float vol = fs35.getVolume();
        bool  mute = fs35.isMuted();
        channelRow(dim ? "Disk 3.5\" (samples missing)" : "Disk 3.5\"",
                   vol, mute,
                   fs35.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs35", dim, &fs35);
        if (!dim) {
            if (vol != fs35.getVolume()) fs35.setVolume(vol);
            if (mute != fs35.isMuted()) fs35.setMuted(mute);
        }
    }

    // ── Printer (synthesised head / platen / carriage) ─────────────────
    // Always shown, like Speaker and Cassette: the source is host-side and
    // not owned by any card, so there is no slot to gate it on. Before this
    // row existed the level and mute persisted in state.cfg but had no
    // in-app control at all — silencing a too-loud printer meant quitting
    // and hand-editing the file.
    {
        float vol  = printerSound->volume();
        bool  mute = printerSound->muted();
        channelRow("Printer", vol, mute,
                   printerSound->lastBufferPeak.load(std::memory_order_relaxed),
                   "prn", false, printerSound.get());
        if (vol != printerSound->volume()) printerSound->setVolume(vol);
        if (mute != printerSound->muted()) printerSound->setMuted(mute);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Master is post-mix; per-channel knobs are pre-mix.");
    ImGui::TextDisabled("Bars show last-buffer peak with ~100 ms release.");
    ImGui::TextDisabled("AY cards pan themselves per chip (MAME wiring).");
    ImGui::End();
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!uiState_->showChatMauvePanel) return;

    const auto snap = devicePanelCoordinator_->captureChatMauve();

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel->render("Le Chat Mauve###chatMauvePanel",
                                        uiState_->showChatMauvePanel, snap);

    devicePanelCoordinator_->applyChatMauve(result);
}

// ─── Mockingboard live state panel ───────────────────────────────────────
//
// Diagnostic window for the Mockingboard A/C. Shows both 6522 VIAs' T1
// counter / IFR / IER + slot IRQ state, and both AY-3-8910 register
// banks (R0..R15). Primary use case: figuring out why an IRQ-driven
// music driver is silent. Three observable cases:
//
//   1. AY registers all stay 0 — the music handler isn't running at
//      all. Check IFR/IER + irqAsserted on the VIA. If T1 ticks but
//      IRQ never asserts, the IER is wrong or the driver hasn't
//      enabled T1 yet.
//   2. AY registers move every few frames — the driver is running and
//      the AY synth is producing samples; if you hear nothing, look at
//      AudioDevice (volume/mute) or the channel mixer R7.
//   3. AY registers load once and freeze — the install ran but only
//      one IRQ landed. Likely the handler isn't re-arming T1 or the
//      ack path is broken.
//
// The panel takes the controller state mutex for each snapshot and
// reads via the card's existing test/debug accessors
// (`peekViaRegister`, `getAyRegister`, `isIrqAsserted`).
void MainWindow::renderMockingboardPanelWindow()
{
    if (!uiState_->showMockingboardPanel) return;

    const auto mbSnapshot = audioCoordinator_->captureMockingboard();

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mockingboard (VIA + AY state)",
                      &uiState_->showMockingboardPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!mbSnapshot.plugged) {
        ImGui::TextDisabled("No Mockingboard plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    const auto& via = mbSnapshot.via;

    // Slot IRQ indicator and volume readout.
    ImGui::Text("Slot %d IRQ line: %s", mbSnapshot.slot,
                mbSnapshot.slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        mbSnapshot.volume,
                        mbSnapshot.muted ? "(MUTED)" : "");
    ImGui::Separator();

    // Two-column display, one per 6522+AY pair.
    if (ImGui::BeginTable("##mb_chips", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 + AY #0 ($Cn00-$Cn0F)");
        ImGui::TableSetupColumn("VIA #2 + AY #1 ($Cn80-$Cn8F)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            // Telemetry first — these counters tell you instantly
            // whether the music driver is even talking to this VIA.
            // viaWrites grows on every guest STA $CnXX (Mockingboard
            // slot-ROM window); ayWrites grows only when LATCH→WRITE
            // strobes successfully complete and a register lands in
            // the AY. Both staying near zero mid-game = the driver
            // isn't running or its IRQ handler is short-circuiting
            // before the AY phase.
            ImGui::Text("VIA writes : %u", v.viaWrites);
            ImGui::Text("AY writes  : %u  (register-store WRITE strobes)",
                        v.ayWrites);
            ImGui::Text("AY resets  : %u  (!RESET pulses, wipes all regs)",
                        v.ayResets);
            ImGui::TextDisabled("AY cmd:  LATCH=%u WRITE=%u INACT=%u READ=%u",
                                v.cmdLatch, v.cmdWrite, v.cmdInactive,
                                v.cmdRead);
            ImGui::Separator();
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
            ImGui::Separator();
            ImGui::TextDisabled("AY-3-8910 registers:");
            // Two columns inside the cell: 8 regs each.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row;
                const int r1 = row + 8;
                ImGui::Text("R%-2d %3u $%02X    R%-2d %3u $%02X",
                            r0, v.ay[r0], v.ay[r0],
                            r1, v.ay[r1], v.ay[r1]);
            }
            // Friendly labels for the regs that decide audibility.
            ImGui::Separator();
            const uint16_t periodA = v.ay[0] | ((v.ay[1] & 0x0F) << 8);
            const uint16_t periodB = v.ay[2] | ((v.ay[3] & 0x0F) << 8);
            const uint16_t periodC = v.ay[4] | ((v.ay[5] & 0x0F) << 8);
            ImGui::Text("Ch A period $%03X  vol $%X", periodA, v.ay[8] & 0x1F);
            ImGui::Text("Ch B period $%03X  vol $%X", periodB, v.ay[9] & 0x1F);
            ImGui::Text("Ch C period $%03X  vol $%X", periodC, v.ay[10] & 0x1F);
            ImGui::Text("Mixer $%02X (tone %c%c%c noise %c%c%c)",
                        v.ay[7],
                        (v.ay[7] & 0x01) ? '.' : 'A',
                        (v.ay[7] & 0x02) ? '.' : 'B',
                        (v.ay[7] & 0x04) ? '.' : 'C',
                        (v.ay[7] & 0x08) ? '.' : 'A',
                        (v.ay[7] & 0x10) ? '.' : 'B',
                        (v.ay[7] & 0x20) ? '.' : 'C');
        }
        ImGui::EndTable();
    }

    // ── Sound II SSI263 section (only if this variant has one) ─────────
    if (mbSnapshot.hasSsi) {
        const auto& ssiSnap = mbSnapshot.ssi;
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                           "SSI263 (Sound II — speech @ $Cs40-$Cs44)");
        if (ssiSnap.powerDown) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                               "  CTL=1  →  POWER DOWN (silent)");
        }
        ImGui::TextColored(ssiSnap.aRequest
                             ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                             : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  A/!R: %s  →  VIA1.IFR.CA1 if PCR.0=0",
                           ssiSnap.aRequest ? "REQUEST" : "running");
        ImGui::Text("  Phoneme: $%02X (%d)  remaining: %d cyc",
                    ssiSnap.currentPhoneme, ssiSnap.currentPhoneme,
                    ssiSnap.phonemeRemainingCycles);
        ImGui::Text("  Writes since reset: %u  |  IRQ enable: %s",
                    ssiSnap.phonemeWriteCount,
                    ssiSnap.irqEnabled ? "yes" : "no");
        ImGui::Text("  Regs: $00=%02X  $01=%02X  $02=%02X  $03=%02X  $04=%02X",
                    ssiSnap.regs[0], ssiSnap.regs[1], ssiSnap.regs[2],
                    ssiSnap.regs[3], ssiSnap.regs[4]);
    }

    ImGui::End();
}

// ─── Phasor ──────────────────────────────────────────────────────────────

void MainWindow::renderPhasorPanelWindow()
{
    if (!uiState_->showPhasorPanel) return;

    const auto phSnapshot = audioCoordinator_->capturePhasor();

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Phasor (mode + 2×VIA + 4×AY)",
                      &uiState_->showPhasorPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!phSnapshot.plugged) {
        ImGui::TextDisabled("No Phasor plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    const auto& via = phSnapshot.via;
    const auto& ay = phSnapshot.ay;
    const auto mode = static_cast<PhasorCard::Mode>(phSnapshot.mode);
    const int clockScale = phSnapshot.clockScale;

    // ── Mode banner ─────────────────────────────────────────────────────
    const char* modeLabel =
        (mode == PhasorCard::PH_Phasor)       ? "PHASOR NATIVE"  :
        (mode == PhasorCard::PH_EchoPlus)     ? "ECHO+"          :
        (mode == PhasorCard::PH_Mockingboard) ? "MOCKINGBOARD"   :
                                                "(unknown)";
    const ImVec4 modeColor =
        (mode == PhasorCard::PH_Phasor)       ? ImVec4(0.30f, 0.85f, 0.45f, 1.0f) :
        (mode == PhasorCard::PH_EchoPlus)     ? ImVec4(0.40f, 0.65f, 0.95f, 1.0f) :
                                                ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    ImGui::TextColored(modeColor, "Mode: %s", modeLabel);
    ImGui::SameLine();
    ImGui::TextDisabled("  (clock ×%d, %d AYs active)",
                        clockScale,
                        (mode == PhasorCard::PH_Mockingboard) ? 2 : 4);
    ImGui::Text("Slot IRQ line: %s",
                phSnapshot.slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        phSnapshot.volume,
                        phSnapshot.muted ? "(MUTED)" : "");
    {
        // Device-select page for this slot is $C0n0..$C0nF where
        // n = 8 + slot (e.g. slot 4 → $C0C0..$C0CF). The mode soft-
        // switch responds to read OR write of those 16 addresses.
        const int devHi = 0x8 + phSnapshot.slot;
        ImGui::TextDisabled(
            "Mode soft-switch: read/write $C0%X8 → MB, $C0%XD → Phasor",
            devHi, devHi);
    }
    ImGui::Separator();

    // ── VIA telemetry (2 cols) ─────────────────────────────────────────
    if (ImGui::BeginTable("##ph_vias", 2,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 ($Cs00-$Cs0F) → AY0 / AY1");
        ImGui::TableSetupColumn("VIA #2 ($Cs80-$Cs8F) → AY2 / AY3");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            ImGui::Text("VIA writes : %u", v.writes);
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    // ── AY-3-8913 register banks (4 cols) ─────────────────────────────
    if (ImGui::BeginTable("##ph_ays", 4,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("AY0 (VIA1 pri)");
        ImGui::TableSetupColumn("AY1 (VIA1 sec)");
        ImGui::TableSetupColumn("AY2 (VIA2 pri)");
        ImGui::TableSetupColumn("AY3 (VIA2 sec)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& a = ay[c];
            // In MB-compat mode the secondary AYs (1, 3) are unreachable
            // — flag the cell so the user understands why the bank
            // stays at zero even with a music driver running.
            const bool unreachableInMb =
                (mode == PhasorCard::PH_Mockingboard) && ((c & 1) != 0);
            if (unreachableInMb) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.4f, 1.0f),
                                   "(MB-compat: silent)");
            }
            ImGui::Text("writes %u", a.writes);
            ImGui::Text("resets %u", a.resets);
            ImGui::Separator();
            // Compact reg dump: 8 rows × 2 regs per cell.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row, r1 = row + 8;
                ImGui::Text("R%-2d $%02X  R%-2d $%02X",
                            r0, a.regs[r0], r1, a.regs[r1]);
            }
            ImGui::Separator();
            const uint16_t periodA = a.regs[0] | ((a.regs[1] & 0x0F) << 8);
            const uint16_t periodB = a.regs[2] | ((a.regs[3] & 0x0F) << 8);
            const uint16_t periodC = a.regs[4] | ((a.regs[5] & 0x0F) << 8);
            ImGui::Text("A $%03X v$%X", periodA, a.regs[8]  & 0x1F);
            ImGui::Text("B $%03X v$%X", periodB, a.regs[9]  & 0x1F);
            ImGui::Text("C $%03X v$%X", periodC, a.regs[10] & 0x1F);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ─── Echo+ ───────────────────────────────────────────────────────────────

void MainWindow::renderEchoPlusPanelWindow()
{
    if (!uiState_->showEchoPlusPanel) return;

    const auto echoSnapshot = audioCoordinator_->captureEchoPlus();

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Echo+ (SSI263 speech)", &uiState_->showEchoPlusPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!echoSnapshot.plugged) {
        ImGui::TextDisabled("No Echo+ plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    const auto& s = echoSnapshot.chip;

    // ── Header ────────────────────────────────────────────────────────
    ImGui::Text("Slot %d  |  Volume %.2f %s",
                echoSnapshot.slot,
                echoSnapshot.volume,
                echoSnapshot.muted ? "(MUTED)" : "");
    ImGui::TextColored(echoSnapshot.slotIrq
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "Slot IRQ: %s",
                       echoSnapshot.slotIrq ? "ASSERTED (low)" : "released");
    ImGui::Separator();

    // ── Live chip state ───────────────────────────────────────────────
    if (s.powerDown) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                           "CTL=1  →  POWER DOWN (silent)");
    } else {
        ImGui::Text("CTL=0  →  RUN");
    }
    const char* modeStr =
        (s.mode == pom2::Ssi263::MODE_IRQ_DISABLED)                    ? "00 IRQ disabled" :
        (s.mode == pom2::Ssi263::MODE_FRAME_IMMEDIATE_INFLECTION)      ? "01 Frame imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION)    ? "10 Phon. imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION) ? "11 Phon. trans. infl." :
                                                                          "??";
    ImGui::Text("Mode: %s  |  IRQ enable: %s",
                modeStr, s.irqEnabled ? "yes" : "no");
    ImGui::TextColored(s.aRequest
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "A/!R: %s", s.aRequest ? "REQUEST (phoneme done)"
                                              : "running");
    ImGui::Text("Current phoneme: $%02X (%d)",
                s.currentPhoneme, s.currentPhoneme);
    ImGui::Text("Duration remaining: %d cycles (≈ %.1f ms)",
                s.phonemeRemainingCycles,
                s.phonemeRemainingCycles / 1022.727);
    ImGui::Text("Phoneme writes since reset: %u",
                s.phonemeWriteCount);
    ImGui::Separator();

    // ── Register dump ──────────────────────────────────────────────────
    ImGui::TextDisabled("SSI263 registers ($Cs00-$Cs04):");
    const char* labels[5] = {
        "$00 DURPHON", "$01 INFLECT", "$02 RATEINF",
        "$03 CTTRAMP", "$04 FILFREQ"
    };
    for (int r = 0; r < 5; ++r) {
        ImGui::Text("%s = $%02X (%3u)", labels[r], s.regs[r], s.regs[r]);
    }
    ImGui::Separator();

    // ── Status footer ──────────────────────────────────────────────────
    ImGui::TextWrapped(
        "Audio: live — 62-phoneme PCM blob (ported from AppleWin) "
        "resampled from 22050 Hz to the host rate, scaled by the AMP "
        "register. Power-down (CTL=1) or FILFREQ=$FF squelches output.");

    ImGui::End();
}

// ─── Disk II ─────────────────────────────────────────────────────────────
