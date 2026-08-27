// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MainWindow_MiscPanels — the ImGui bodies for the memory viewer, the
// cassette deck, the HGR/DHGR paint and sprite editors, and the rewind
// timeline. Moved out of MainWindow.cpp verbatim. (The keyboard and welcome
// panels stay in MainWindow.cpp: they load a texture via the stb_image
// instance defined there under STB_IMAGE_STATIC, whose symbols are internal
// to that translation unit.)

#include "MainWindow.h"
#include "DebugCoordinator.h"
#include "EmulationController.h"

#include "CassetteDeck_ImGui.h"
#include "CassetteDevice.h"
#include "MemoryViewer_ImGui.h"
#include "Pom2HgrPaintHost.h"
#include "hgrpaint/HgrPaintEditor.h"
#include "hgrsprite/HgrSpriteEditor.h"
#include "Rewind_ImGui.h"
#include "Settings.h"

#include "imgui.h"

void MainWindow::renderMemoryViewerWindow()
{
    // The whole body — window, locked snapshot, and the flush that MUST
    // happen after the lock is released — belongs to DebugCoordinator. The
    // ordering is the reason it is one unit: the write sink re-takes
    // stateMutex to push each poke through Memory::memWrite like a CPU store,
    // and that mutex is non-recursive, so flushing inline would freeze the UI
    // thread while it still holds the lock the worker needs at its next chunk.
    debugCoordinator_->renderMemoryViewer(show(pom2::PanelId::MemViewer));
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!show(pom2::PanelId::Cassette)) return;

    // Build the deck snapshot under stateMutex — cheap enough that holding
    // the emul lock for the time it takes to copy a dozen scalars is fine.
    pom2::CassetteDeck_ImGui::DeckSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const CassetteDevice& d = controller->cassette();
        snap.loadedTape          = d.hasLoadedTape();
        snap.recordedTape        = d.hasRecordedTape();
        snap.playbackActive      = d.isPlaybackActive();
        snap.playbackArmed       = d.isPlaybackArmed();
        snap.rewinding           = d.isRewinding();
        snap.audioAvailable      = d.isAudioAvailable();
        snap.playbackPaused      = d.isPlaybackPaused();
        snap.audioStreamMode     = d.isAudioStreamMode();
        snap.queuedAudioSeconds  = d.getQueuedAudioSeconds();
        snap.playbackPositionSec = d.getPlaybackPositionSeconds();
        snap.playbackTotalSec    = d.getPlaybackTotalSeconds();
        snap.loadedTransitions   = d.getLoadedTransitionCount();
        snap.recordedTransitions = d.getRecordedTransitionCount();
        snap.volume              = d.getVolume();
        snap.loadedTapePath      = d.getLoadedTapePath();
        snap.loadInfo            = d.getLoadInfo();
    }

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    auto result = cassetteDeck->render("Cassette Deck",
                                      show(pom2::PanelId::Cassette),
                                      controller.get(),
                                      snap,
                                      deltaSeconds);

    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 4.0;  // show for 4 seconds
    }
}

void MainWindow::renderHgrPaintWindow()
{
    if (!show(pom2::PanelId::HgrPaint)) return;

    // 64 KB main-RAM (+ IIe aux) snapshot under stateMutex — the editor's
    // per-frame canvas/shadow read source (same idiom as the deck snapshot).
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }

    const float w = hgrpaint::kHiresWidth  * 3.0f + 40.0f;
    const float h = hgrpaint::kHiresHeight * 3.0f + 180.0f;
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Paint Editor", &show(pom2::PanelId::HgrPaint)))
        hgrPaintEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                  : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::renderHgrSpriteWindow()
{
    if (!show(pom2::PanelId::HgrSprite)) return;
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Sprite Editor", &show(pom2::PanelId::HgrSprite)))
        hgrSpriteEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                   : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::driveRewindHold(bool held)
{
    // Edge-detect: hold → step the machine backwards one frame; release →
    // resume live from the rewound point. No-op when recording is off
    // (holdRewind/beginScrub bail out), so it never surprises a non-user.
    if (held)                  rewindPanel_->holdRewind(*controller, 1);
    else if (rewindHeldPrev_)  rewindPanel_->releaseHold(*controller);
    rewindHeldPrev_ = held;
}

void MainWindow::renderRewindWindow(float deltaSeconds)
{
    if (!show(pom2::PanelId::Rewind)) return;
    auto result = rewindPanel_->render("Rewind", show(pom2::PanelId::Rewind), *controller, deltaSeconds);
    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
}
