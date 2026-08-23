// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MainWindow_DevicePanels — the ImGui bodies for the non-audio device panels:
// Ethernet (Uthernet I/II), the Super Serial card, the parallel printer, the
// ImageWriter tray and the AI control server panel. Moved out of
// MainWindow.cpp verbatim; none touch the anonymous-namespace helpers that
// keep the storage panels there. See the file-size ratchet.

#include "MainWindow.h"
#include "EmulationController.h"

#include "AiControlServer.h"
#include "AtomicFileReplace.h"
#include "FujiNetCard.h"
#include "ImageWriter_ImGui.h"
#include "Logger.h"
#include "PrinterFeedCursor.h"
#include "PrinterHistory.h"
#include "ResourcePaths.h"
#include "SlirpNetworkBackend.h"
#include "Uthernet_ImGui.h"
#include "GrapplerCard.h"
#include "ImageWriter.h"
#include "PrinterCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

#include "imgui.h"

// Ethernet (Uthernet I / II). Snapshot-under-lock → render → dispatch
// actions under the lock again, the LeChatMauve_ImGui pattern. The card
// pointers are non-owning (SlotBus owns the cards) and are nulled on
// every re-plug, so they are only ever dereferenced inside the lock.
void MainWindow::renderEthernetPanelWindow()
{
    if (!show(pom2::PanelId::Ethernet)) return;
    if (!ethernetPanel) ethernetPanel = std::make_unique<pom2::Uthernet_ImGui>();

    pom2::Uthernet_ImGui::Snapshot snap;
    snap.slirpCompiledIn = pom2::slirpAvailable();
    snap.backendChoice   = settings->getString("ethernet_backend", "slirp");

    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());

        if (uthernetCard) {
            const pom2::Cs8900aDevice& chip = uthernetCard->chip();
            const pom2::NetworkBackend* be  = uthernetCard->backend();
            snap.u1Plugged        = true;
            snap.u1Slot           = uthernetCard->getSlot();
            snap.u1Backend        = be ? std::string(be->name()) : "none";
            snap.u1BackendValid   = be && be->isValid();
            snap.u1Mac            = chip.macAddress();
            snap.u1RxEnabled      = chip.receiverEnabled();
            snap.u1TxEnabled      = chip.transmitterEnabled();
            snap.u1Promiscuous    = chip.promiscuous();
            snap.u1PacketPagePtr  = chip.packetPagePointer();
            snap.u1Queued         = chip.queuedFrames();
            snap.u1FramesSent     = chip.framesSent();
            snap.u1FramesReceived = chip.framesReceived();
            snap.u1FramesFiltered = chip.framesFiltered();
        }

        if (uthernetIICard) {
            const pom2::W5100Device& chip   = uthernetIICard->chip();
            const pom2::NetworkBackend* be  = uthernetIICard->backend();
            snap.u2Plugged        = true;
            snap.u2Slot           = uthernetIICard->getSlot();
            snap.u2Backend        = be ? std::string(be->name()) : "none";
            snap.u2BackendValid   = be && be->isValid();
            snap.u2Mac            = chip.macAddress();
            snap.u2Ip             = chip.localIp();
            snap.u2VirtualDns     = chip.virtualDnsEnabled();
            snap.u2BytesSent      = chip.bytesSent();
            snap.u2BytesReceived  = chip.bytesReceived();
            for (size_t i = 0; i < pom2::W5100Device::kSocketCount; ++i)
                snap.u2Sockets[i] = chip.socketInfo(i);
        }
    }

    const auto action =
        ethernetPanel->render("Ethernet###ethernetPanel", show(pom2::PanelId::Ethernet), snap);

    if (action.requestResetU1 || action.requestResetU2 ||
        action.requestVirtualDns) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (action.requestResetU1 && uthernetCard) uthernetCard->onReset();
        if (action.requestResetU2 && uthernetIICard) uthernetIICard->onReset();
        if (action.requestVirtualDns && uthernetIICard) {
            uthernetIICard->chip().setVirtualDnsEnabled(action.virtualDnsTo);
            settings->setBool("uthernet2_virtual_dns", action.virtualDnsTo);
        }
    }
}

void MainWindow::renderSscPanelWindow()
{
    if (!show(pom2::PanelId::Ssc) || sscCards.empty()) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Super Serial###sscPanel", &show(pom2::PanelId::Ssc))) {
        ImGui::End();
        return;
    }

    // One panel hosts every plugged SSC under a tab bar. //c boots with
    // two (printer + modem); other profiles typically run zero or one.
    // Per-slot port-input state lives in a static map so each tab keeps
    // its own draft port number across frames.
    auto renderOne = [&](SuperSerialCard* ssc) {
        if (!ssc) return;
        const int slot = ssc->getSlot();
        static std::map<int, int> portDrafts;
        auto it = portDrafts.find(slot);
        if (it == portDrafts.end()) {
            portDrafts[slot] = ssc->getPort() ? ssc->getPort()
                : SuperSerialCard::kDefaultPort;
            it = portDrafts.find(slot);
        }
        int& portDraft = it->second;

        const bool listening = ssc->isListening();
        const bool connected = ssc->clientConnected();

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
                if (!ssc->startListening(static_cast<uint16_t>(portDraft))) {
                    tapeStatusMessage = "SSC slot " + std::to_string(slot) +
                        ": bind failed (port busy?)";
                    tapeStatusUntil   = lastFrameTime + 4.0;
                }
            }
        } else {
            if (ImGui::Button("Stop listener")) ssc->stopListening();
        }

        if (listening) {
            ImGui::TextWrapped("Connect from a host terminal:");
            ImGui::TextWrapped("  telnet 127.0.0.1 %d", ssc->getPort());
            ImGui::TextWrapped("In the Apple II:  PR#%d  (or IN#%d for input)",
                slot, slot);
        } else {
            ImGui::TextDisabled("Click Start, then telnet to the port to "
                                "bridge I/O between your host shell and "
                                "the Apple II.");
        }
#endif

        ImGui::Separator();
        bool raw = ssc->rawMode();
        if (ImGui::Checkbox("Raw mode (8-bit binary)", &raw)) {
            ssc->setRawMode(raw);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: stock telnet — IAC ($FF) negotiation\n"
                              "swallowed + CR/LF normalised to CR.\n"
                              "On: every byte forwarded verbatim. Use for\n"
                              "XMODEM / Kermit / ADTPro / any binary protocol.");
        }

        bool tap = ssc->printerTap();
        if (ImGui::Checkbox("Feed ImageWriter printer", &tap)) {
            ssc->setPrinterTap(tap);
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
            static_cast<unsigned long long>(ssc->bytesRx()));
        ImGui::Text("TX (A2 → telnet): %llu B",
            static_cast<unsigned long long>(ssc->bytesTx()));

        if (ImGui::CollapsingHeader("Recent traffic")) {
            ImGui::TextDisabled("Last bytes the Apple II printed via PR#%d:",
                                slot);
            ImGui::TextWrapped("%s", ssc->recentTxText().c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Last bytes the host typed:");
            ImGui::TextWrapped("%s", ssc->recentRxText().c_str());
        }
        ImGui::PopID();
    };

    if (sscCards.size() == 1) {
        renderOne(sscCards[0]);
    } else if (ImGui::BeginTabBar("##sscTabs")) {
        // //c convention: sl1 = printer port, sl2 = modem port. Other
        // profiles just label by slot number.
        const bool isIIcLayout = (sscCards.size() == 2) &&
            (sscCards[0]->getSlot() == 1) && (sscCards[1]->getSlot() == 2);
        for (size_t i = 0; i < sscCards.size(); ++i) {
            const int slot = sscCards[i]->getSlot();
            std::string tab;
            if (isIIcLayout) tab = (i == 0) ? "Printer port (sl1)"
                                            : "Modem port (sl2)";
            else             tab = "Slot " + std::to_string(slot);
            if (ImGui::BeginTabItem(tab.c_str())) {
                renderOne(sscCards[i]);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void MainWindow::renderPrinterPanelWindow()
{
    if (!show(pom2::PanelId::Printer) || !printerCard) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    const std::string title = "Printer (slot " +
        std::to_string(printerCard->getSlot()) + ")###printerPanel";
    if (!ImGui::Begin(title.c_str(), &show(pom2::PanelId::Printer))) {
        ImGui::End();
        return;
    }

    const size_t nBytes = printerCard->bytesWritten();
    ImGui::Text("Spool: %zu byte%s", nBytes, nBytes == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("— PR#%d from BASIC sends output here",
                        printerCard->getSlot());
    if (printerCard->spoolTruncated()) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
            "Preview/save retains only the newest %zu bytes; older output "
            "was already streamed to the virtual printer.",
            PrinterCard::kMaxSpoolBytes);
    }

    ImGui::Separator();

    // Auto-suggest a timestamped path on first open so the user can hit
    // Save without typing anything. printerSavePath persists across saves
    // within a session — the user can edit it freely.
    if (printerSavePath.empty()) {
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
        printerSavePath = (pom2::userDataDir() / "printouts" /
                           (std::string("spool-") + stamp + ".txt")).string();
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", printerSavePath.c_str());
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
        printerSavePath = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as .txt", ImVec2(100, 0))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::path(printerSavePath);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        const fs::path tmp = p.string() + ".pom2tmp";
        std::error_code permEc;
        const fs::perms oldPerms = fs::status(p, permEc).permissions();
        const bool havePerms = !permEc;
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            printerLastSaveStatus = "Save failed: cannot open " +
                                    p.string();
        } else {
            const std::string text = printerCard->spoolText();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            out.close();
            if (!out) {
                printerLastSaveStatus = "Save failed: short write to " +
                                        tmp.string();
                fs::remove(tmp, ec);
            } else {
                if (havePerms) {
                    fs::permissions(tmp, oldPerms, ec);
                    ec.clear();
                }
                if (!pom2::replaceFileAtomic(tmp, p, ec)) {
                    printerLastSaveStatus = "Save failed: cannot replace " +
                                            p.string() + ": " + ec.message();
                    fs::remove(tmp, ec);
                } else {
                    printerLastSaveStatus = "Saved " +
                        std::to_string(text.size()) + " bytes → " + p.string();
                }
            }
        }
    }

    if (!printerLastSaveStatus.empty()) {
        ImGui::TextDisabled("%s", printerLastSaveStatus.c_str());
    }
#endif

    if (ImGui::Button("Clear spool")) {
        printerCard->clearSpool();
        printerLastSaveStatus.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(host-side buffer only — does NOT touch the Apple II)");

    ImGui::Separator();
    ImGui::TextDisabled("Preview (high bit stripped, CR → LF):");

    // Snapshot once per frame; printerCard mutates this from the CPU
    // thread, but we hold the state lock everywhere it's touched so a
    // single read is consistent.
    const std::string preview = printerCard->spoolText();
    ImGui::BeginChild("##printerPreview", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (preview.empty()) {
        ImGui::TextDisabled("(empty — try `PR#%d : PRINT \"HELLO\"` from BASIC)",
                            printerCard->getSlot());
    } else {
        ImGui::TextUnformatted(preview.data(),
                               preview.data() + preview.size());
    }
    ImGui::EndChild();

    ImGui::End();
}

SuperSerialCard* MainWindow::printerTapSsc() const
{
    // sscCards is sorted by slot ascending, so the first tapped card is
    // the lowest slot — the //c printer port when that profile is active.
    for (auto* ssc : sscCards)
        if (ssc && ssc->printerTap()) return ssc;
    return nullptr;
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

    std::vector<uint8_t> fresh;
    size_t total = 0;
    // Cursor handover rules (source change, spool cleared) live in
    // printerFeedCursor() — see PrinterFeedCursor.h for why re-seating at
    // 0 was wrong. Pinned by testSpoolSeam.
    if (printerCard) {
        imageWriterConsumed = pom2::printerFeedCursor(
            imageWriterSource, imageWriterConsumed,
            printerCard, printerCard->bytesWritten());
        total = printerCard->drainSpoolFrom(imageWriterConsumed, fresh);
    } else if (grapplerCard) {
        imageWriterConsumed = pom2::printerFeedCursor(
            imageWriterSource, imageWriterConsumed,
            grapplerCard, grapplerCard->bytesWritten());
        total = grapplerCard->drainSpoolFrom(imageWriterConsumed, fresh);
    } else if (fujiNetCard && fujiNetCard->hasPrinterUnit()) {
        // FujiNet's printer unit. Ranked below the parallel cards (a machine
        // with a real printer card keeps that routing) but above the SSC tap,
        // because a FujiNet printer is an explicit choice while the tap is a
        // //c-class default. The FujiNet prints its own copy too — POM2 just
        // renders the same byte stream on its own paper.
        imageWriterConsumed = pom2::printerFeedCursor(
            imageWriterSource, imageWriterConsumed,
            fujiNetCard, fujiNetCard->bytesWritten());
        total = fujiNetCard->drainSpoolFrom(imageWriterConsumed, fresh);
    } else if (SuperSerialCard* tap = printerTapSsc()) {
        // //c-class printer port: the SSC's TX tap (slot 1 by default —
        // see plugSlotsFromSettings). Parallel cards outrank it so a IIe
        // with both a PrinterCard and an SSC keeps the parallel routing.
        imageWriterConsumed = pom2::printerFeedCursor(
            imageWriterSource, imageWriterConsumed,
            tap, tap->printerSpoolBytes());
        total = tap->drainPrinterSpoolFrom(imageWriterConsumed, fresh);
    } else {
        // No source this frame (card unplugged, or the tap switched off).
        // Forget WHICH source it was so the next one re-seats — but do not
        // zero the cursor here: that was the other half of the reprint
        // bug, since a source that comes back is re-seated at its own
        // current total by resyncSource above.
        imageWriterSource = nullptr;
        // The mechanism keeps running: a job already in the printer's
        // input buffer must still reach paper even with nothing feeding
        // it (unplugging the card does not un-print the page).
        imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));
        return;
    }

    if (!fresh.empty()) {
        imageWriter->queueBytes(fresh.data(), fresh.size());
        if (imageWriter->tracing())
            imageWriter->traceEvent("card delivered %zu byte%s (queue now %zu)",
                                    fresh.size(), fresh.size() == 1 ? "" : "s",
                                    imageWriter->pendingBytes());
    }
    imageWriterConsumed = total;

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
    if (grapplerCard) {
        // Only hold the line when the user asked for the real handshake:
        // a Print Shop page is ~100 KB of dot columns, which at 250 cps
        // keeps the guest blocked for minutes. Faithful, and
        // indistinguishable from a hang unless you know to expect it.
        const bool busy =
            printerBackPressure &&
            imageWriter->pendingBytes() > pom2::ImageWriter::kInputBufferBytes;
        if (busy != grapplerCard->printerBusy()) {
            grapplerCard->setPrinterBusy(busy);
            if (imageWriter->tracing())
                imageWriter->traceEvent(
                    "BUSY=%d — the Apple II %s (queue %zu / %zu buffer)",
                    busy ? 1 : 0,
                    busy ? "is now waiting on the printer"
                         : "may send again",
                    imageWriter->pendingBytes(),
                    pom2::ImageWriter::kInputBufferBytes);
        }
    }
}

void MainWindow::renderImageWriterWindow()
{
    if (!show(pom2::PanelId::ImageWriter) || !imageWriter || !imageWriterPanel) return;

    pom2::ImageWriter_ImGui::HostInfo host;

    // There is one ImageWriter and up to three things that can feed it, so
    // pumpImageWriter() arbitrates: parallel cards outrank the SSC tap,
    // and PrinterCard outranks Grappler+. Slot Config lets a PrinterCard
    // and a Grappler+ coexist (different catalog keys, so its duplicate
    // check does not object), and the loser then feeds nothing at all —
    // silently, with paper that just stays blank. Name the losers.
    std::vector<std::string> ignored;
    {
        const bool haveParallel = printerCard || grapplerCard;
        if (printerCard && grapplerCard)
            ignored.push_back("Grappler+ slot " +
                              std::to_string(grapplerCard->getSlot()));
        if (haveParallel)
            for (auto* ssc : sscCards)
                if (ssc && ssc->printerTap())
                    ignored.push_back("Super Serial slot " +
                                      std::to_string(ssc->getSlot()));
    }

    if (printerCard) {
        host.haveSource  = true;
        host.sourceLabel = "fed by Printer card, slot " +
                           std::to_string(printerCard->getSlot());
    } else if (grapplerCard) {
        host.haveSource  = true;
        host.sourceLabel = "fed by Grappler+, slot " +
                           std::to_string(grapplerCard->getSlot());
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
        host.cardDipValue = static_cast<int>(grapplerCard->printerType());
        host.cardDipRecommended = static_cast<int>(PT::AppleDotMatrix);
        host.backPressure = printerBackPressure;
        host.onBackPressureChanged =
            [this](bool v) { printerBackPressure = v; };
        host.onCardDipChanged = [this](int v) {
            if (grapplerCard)
                grapplerCard->setPrinterType(
                    static_cast<GrapplerCard::PrinterType>(v));
        };
    } else if (SuperSerialCard* tap = printerTapSsc()) {
        host.haveSource  = true;
        host.sourceLabel = "fed by Super Serial (printer port), slot " +
                           std::to_string(tap->getSlot());
    }
    if (host.haveSource && !ignored.empty()) {
        host.sourceLabel += "  (not feeding: ";
        for (size_t i = 0; i < ignored.size(); ++i)
            host.sourceLabel += (i ? ", " : "") + ignored[i];
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

    imageWriterPanel->render(&show(pom2::PanelId::ImageWriter), *imageWriter, host);
}

void MainWindow::renderAiControlPanelWindow()
{
    if (!show(pom2::PanelId::AiControl)) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Control (HTTP)", &show(pom2::PanelId::AiControl))) {
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
            // Re-attach in case slot cards were rebuilt by the slot config
            // panel since the last start — pointers may have moved.
            aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
            aiServer->setAuthToken(aiTokenInput);
            if (!aiServer->start(static_cast<uint16_t>(aiPortInput))) {
                tapeStatusMessage = "AI Control: bind failed (port busy?)";
                tapeStatusUntil   = lastFrameTime + 4.0;
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
