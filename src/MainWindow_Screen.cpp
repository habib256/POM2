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
#include "MouseCoordinator.h"
#include "NetworkCoordinator.h"
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
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
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
#include "PrinterFeedCursor.h"
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
#include "Mockingboard.h"
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

// Physical MainWindow split: screen, kiosk, pointer routing and host input panels.

void MainWindow::renderScreenWindow()
{
    // Default startup layout. Native keeps room for the Disk Library column;
    // WASM starts with only menu + toolbar + Apple II Screen + status bar.
    // `FirstUseEver` only applies on a fresh install — once the user
    // moves / resizes the window their imgui.ini takes over.
#ifdef __EMSCRIPTEN__
    ImGui::SetNextWindowPos (ImVec2(5,    56),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 760), ImGuiCond_FirstUseEver);
#else
    ImGui::SetNextWindowPos (ImVec2(5,    90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 745), ImGuiCond_FirstUseEver);
#endif

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    // `NoMove` so ImGui's default "drag-from-anywhere" doesn't eat
    // click-and-drag gestures inside the screen (Mouse Card games like
    // A2Desktop need them to reach the guest). `NoCollapse` so a
    // double-click on the title bar doesn't accidentally collapse the
    // window — a single concern at a time. We restore the title-bar
    // drag manually below.
    const ImGuiWindowFlags screenFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Apple II Screen", nullptr, screenFlags)) {
        // ── Manual title-bar drag ─────────────────────────────────────
        // Compute the title bar rect from the public Begin geometry
        // (window pos + size + frame height). When the user clicks
        // inside that strip we latch `uiState_->screenDraggingByTitleBar`; on
        // every subsequent frame we apply `io.MouseDelta` until the
        // button is released. `IsAnyItemActive()` guards against
        // claiming a click that another widget already consumed (e.g.
        // any future title-bar button).
        //
        // Skipped entirely while DOCKED: a docked window has no title bar of
        // its own (it's a tab in the host node) and its position is owned by
        // the dock node. Left enabled, the rect we compute lands on the dock
        // node's tab bar and `SetWindowPos` fights the node every frame —
        // the screen jitters and the tab won't drag out.
        if (!ImGui::IsWindowDocked()) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float  th = ImGui::GetFrameHeight();
            const ImVec2 m  = ImGui::GetIO().MousePos;
            const bool overTitleBar =
                (m.x >= wp.x && m.x <= wp.x + ws.x &&
                 m.y >= wp.y && m.y <= wp.y + th);
            // Do not start dragging when another foreground window covers
            // this title-bar rectangle. ImGui's normal move handling already
            // gives that front window priority; the Apple II Screen's manual
            // title drag must obey the same z-order rule.
            const bool screenWindowHovered = ImGui::IsWindowHovered();
            if (screenWindowHovered && overTitleBar &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemActive()) {
                uiState_->screenDraggingByTitleBar = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                uiState_->screenDraggingByTitleBar = false;
            }
            if (uiState_->screenDraggingByTitleBar) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (d.x != 0.0f || d.y != 0.0f) {
                    ImGui::SetWindowPos(ImVec2(wp.x + d.x, wp.y + d.y));
                }
            }
        }

        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainWindow::drawScreenImage()
{
    uploadScreenTexture();

    // OpenEmulator-style composite path: when the user selected
    // ColorCompositeOE AND Apple2Display produced a 14.318 MHz signal
    // this frame, lazily spin up the NTSC shader and run a single pass
    // that consumes the signal texture and writes an RGBA output we
    // hand to ImGui::Image. The first call compiles the shader; if
    // anything fails (driver lacking GL 3.x, shader compile error, …)
    // we fall back to the regular `screenTexture` for the rest of the
    // session — no crashes, no flicker, just the existing LUT view.
    unsigned int presentTex = screenTexture;
    // The 3D voxel view must sample the decoded COLOUR image, NEVER the CRT
    // glass — scanlines / shadow-mask / barrel warp would bake into the cube
    // grid. Track that tap point separately: it follows the colour pipeline
    // (NTSC demod, OE or otherwise) but stops *before* CrtEffectStack. For all
    // non-OE-GPU modes the colour image already lives in `screenTexture`; the
    // OE-GPU branch below redirects it to the demod output.
    unsigned int voxelSrcTex = screenTexture;
    // Sharp-text override: when the user wants legible text under the
    // composite mode, skip the shader for TEXT scanlines and let the
    // crisp RGB framebuffer go straight to ImGui. Full-screen text uses
    // textSharp; mixed HGR/lo-res/DHGR keeps the demod on the graphics
    // band only — the bottom 4 text rows are patched in frame80 as
    // white/black after demod (mixedCompositeUsesFramebuffer).
    // Use the soft-switch snapshot the render() above actually consumed —
    // re-polling Memory::getDisplayState() here raced the CPU worker (it may
    // have advanced past the rendered frame between the two), flashing one
    // LUT-fallback frame on a text↔graphics switch.
    const auto displayState = display->lastRenderState();
    const bool oeGpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOE;
    const bool oeCpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOECpu;
    const bool oeMode    = oeGpuMode;
    const bool oeFamily  = oeGpuMode || oeCpuMode;
    const bool wantSharpText = ntscFx && ntscFx->getParams().textSharp
                            && displayState.textMode;
    const bool mixedFbPresent = display->mixedCompositeUsesFramebuffer();

    // Compute the on-screen target size up-front so the CRT effect pass can
    // render at native output resolution. That is what lets the scanline /
    // shadow-mask patterns be sampled finely enough to analytically
    // anti-alias (no barrel-warp moiré) and lets ImGui blit the result 1:1
    // (no second resample beat). Same three aspect modes used for the final
    // blit below, which reuses `avail` / `size`.
    const float W = static_cast<float>(Apple2Display::kWidth);
    const float H = static_cast<float>(Apple2Display::kHeight);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size;
    switch (aspectMode) {
        case AspectMode::Crt43: {
            float h = std::min(avail.y, avail.x * 3.0f / 4.0f);
            h = std::max(h, H);
            size = ImVec2(h * 4.0f / 3.0f, h);
            break;
        }
        case AspectMode::Integer: {
            float s = std::max(1.0f, std::floor(std::min(avail.x / W, avail.y / H)));
            size = ImVec2(W * s, H * s);
            break;
        }
        case AspectMode::Square:
        default: {
            float s = std::max(1.0f, std::min(avail.x / W, avail.y / H));
            size = ImVec2(W * s, H * s);
            break;
        }
    }
    const int dstW = std::max(1, static_cast<int>(size.x + 0.5f));
    const int dstH = std::max(1, static_cast<int>(size.y + 0.5f));

    if (oeMode && display->signalProduced() && !wantSharpText && !mixedFbPresent) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        if (!ntscFx->available() && !ntscFx->initialize()) {
            // initialize() already logged the failure. Stop trying so
            // we don't spam the log every frame.
        }
        if (ntscFx->available()) {
            // Phase 4 final: NtscPostProcessor is now demod-only (colour
            // recovery). The CRT glass (scanlines / mask / barrel /
            // persistence / BCS) lives in the shared CrtEffectStack, so OE
            // chains into it just like every other mode — one effects
            // implementation. Run the stack ALWAYS for OE (not gated by the
            // opt-in toggle) so the look the user configured is preserved.
            unsigned int demod = 0;
            {
                // demodMutex: `signal()` hands the shader a raw pointer into
                // signalBuf, which the AI control server's /screen handler
                // rewrites via display->render() on its own thread. Without
                // the lock the upload can read a half-rewritten field (one
                // torn frame). uploadScreenTexture() released the lock
                // before returning, so this is a fresh acquisition, not a
                // nesting; scoped tight so the CRT stack below runs unlocked.
                std::lock_guard<std::mutex> demodLk(display->demodMutex());
                demod = ntscFx->process(
                    display->signal(),
                    display->signalWidth(),
                    display->signalHeight(),
                    display->signalPhaseOffset());
            }
            if (demod != 0) {
                presentTex = demod;
                voxelSrcTex = demod;   // colour image, pre-CRT-glass (3D tap)
                // CRT glass only when the master toggle is on (top of the CRT
                // Settings window); otherwise present the raw demod output.
                if (crtEffectsEnabled) {
                    if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
                    if (!crtFx->available()) crtFx->initialize();
                    if (crtFx->available()) {
                        // The OE demod shader already applied hue + chroma-
                        // bandwidth sharpness; zero hue and neutralise sharpness
                        // here so the shared stack doesn't rotate the chroma or
                        // sharpen a second time (0.5 = the stack's neutral pass).
                        pom2::NtscParams crtP = ntscFx->getParams();
                        crtP.hue       = 0.0f;
                        crtP.sharpness = 0.5f;
                        crtFx->setParams(crtP);
                        const unsigned int out = crtFx->process(
                            demod, ntscFx->outputWidth(), ntscFx->outputHeight(),
                            dstW, dstH);
                        if (out != 0) presentTex = out;
                    }
                }
            }
        }
    }

    // OE CPU demod writes frame80 → screenTexture; the OE-GPU fallbacks
    // (mixed graphics+text frame, sharp text, shader unavailable, demod
    // failure) also still present screenTexture at this point. Give ALL of
    // them the same CRT glass — gating on oeCpuMode alone made scanlines /
    // mask / persistence vanish on exactly the mixed frames (score bands,
    // menus, BASIC) and pop back on full-screen graphics, with a stale-
    // persistence ghost on re-entry. `presentTex == screenTexture` is
    // precisely "no OE branch above produced a processed texture".
    if (oeFamily && crtEffectsEnabled && presentTex == screenTexture) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // Neutralise the demod-stage knobs, same as the GPU branch above:
            // the OE-CPU demod (and the mixed-frame CPU demod band) now
            // applies hue + chroma-bandwidth sharpness itself via
            // setOeDemodParams, so the stack must not rotate the chroma or
            // sharpen a second time. (The only frames reaching here without
            // a demod are crisp B/W text — hue is a no-op on grays — and the
            // shader-unavailable LUT fallback, a documented degraded path.)
            pom2::NtscParams crtP = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
            crtP.hue       = 0.0f;
            crtP.sharpness = 0.5f;
            crtFx->setParams(crtP);
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // Universal CRT effect stack (Phase 3): for every NON-OE colour mode,
    // run the framebuffer through the shared scanline / mask / barrel /
    // persistence / BCS pass so those effects work on Color NTSC, Mono,
    // Chat Mauve and AppleWin too. OE GPU and OE CPU share one CRT branch
    // (demod hue/sharpness applied by their demod stage — neutralised there).
    if (!oeFamily && crtEffectsEnabled) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // One CRT Settings panel drives both processors: mirror the
            // NtscParams the user edits there (the demod-only knobs are
            // ignored by the effect stack).
            if (ntscFx) crtFx->setParams(ntscFx->getParams());
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // 3D voxel view: rebuild the decoded COLOUR image (`voxelSrcTex`, the tap
    // taken before CrtEffectStack — so the cubes never inherit scanlines / mask
    // / barrel) as an upright 4:3 slab of equal-depth cubes (MicroM8 "Voxel
    // Cube"), viewed by the orbit camera. Renders at the on-screen size so the
    // aspect is exact; replaces the flat blit (CRT glass computed above is
    // discarded when the 3D view wins). Falls back to the flat texture if the
    // GL renderer can't initialise.
    if (uiState_->show3dVoxel) {
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // One voxel per live Apple II pixel (280 or 560 × 192) so the cube grid
        // captures the full image — half-res sampling visibly lost detail.
        voxel3d_->gridW = std::max(1, display->width());
        voxel3d_->gridH = std::max(1, display->height());
#if defined(__EMSCRIPTEN__)
        // Perf guard: halve the 560-wide DHGR/80-col geometry on the browser so
        // the cube count stays near the comfortable HGR 280×192 (~54k); the FBO
        // supersample is likewise capped in Voxel3DRenderer::process.
        if (voxel3d_->gridW > 280) voxel3d_->gridW = 280;
#endif
        const int vw = std::max(16, static_cast<int>(size.x));
        const int vh = std::max(16, static_cast<int>(size.y));
        const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
        const unsigned int out =
            voxel3d_->process(voxelSrcTex, vw, vh, voxelCam_.viewProj(aspect));
        if (out != 0) presentTex = out;
    }

    // Scale to the content region, then centre (letterbox on a wider/taller
    // region, e.g. a kiosk viewport). `avail` / `size` were computed up-front
    // (above) so the CRT effect pass could render at this exact resolution.
    // The 280×192 active area drove a 4:3 CRT, so its pixels are not square —
    // three presentation modes:
    //   Square  — 1:1 logical pixels (280:192 ≈ 1.46); crisp; never < 1×.
    //   Crt43   — stretch the active area to a true 4:3 frame (real-monitor
    //             shape); fills the region, letterboxed.
    //   Integer — Square snapped to an integer multiple (no fractional-scale
    //             shimmer); never < 1×.
    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
        cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

    ImGui::Image(static_cast<ImTextureID>(presentTex), size);
    // Capture the screen widget's screen-space rect so the GLFW
    // cursor-pos callback (Phase 5) can map a host position onto Apple
    // pixels. The rect answers "*where* in the screen", never "is the
    // screen the one being pointed at" — see uiState_->screenHovered below.
    uiState_->screenRectMin = ImGui::GetItemRectMin();
    uiState_->screenRectMax = ImGui::GetItemRectMax();
    // ...and ImGui's own z-order aware verdict on whether the pointer is
    // actually on the screen widget, which is what decides *ownership* of a
    // click (mouseGrabContext). Unlike the rect, this is false while a
    // dropdown, popup or panel is drawn over the screen, so a click aimed at
    // an open menu no longer doubles as a click into the guest — nor as the
    // capturing press that would steal the pointer behind that menu.
    // Recomputed every frame; renderFrame clears it first so a collapsed or
    // hidden screen window cannot leave a stale `true` behind.
    const bool screenHovered = ImGui::IsItemHovered();
    uiState_->screenHovered = screenHovered;

    // 3D voxel view camera: left-drag orbits, middle-drag strafes (pan),
    // wheel zooms (MicroM8-style). All reference the Image item above
    // (IsItemHovered), so this must stay right after it. Mutates the
    // persistent `voxelCam_` the renderer reads.
    if (uiState_->show3dVoxel && screenHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            voxelCam_.azimuth   += d.x * 0.008f;
            voxelCam_.elevation += d.y * 0.008f;
            const float lim = 1.5f;   // ~86°: stay off the lookAt up-vector poles
            voxelCam_.elevation = std::clamp(voxelCam_.elevation, -lim, lim);
        }
        // Middle-drag = pan/strafe. Scale to world-units-per-pixel at the
        // target plane so the grab tracks the cursor 1:1, and grab the scene
        // (drag right → scene follows right → camera slides left).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            const float k = 2.0f * voxelCam_.distance *
                            std::tan(voxelCam_.fovY * 0.5f) /
                            std::max(1.0f, size.y);
            voxelCam_.pan(-d.x * k, d.y * k);
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            voxelCam_.distance =
                std::clamp(voxelCam_.distance * std::pow(0.9f, wheel), 0.6f, 20.0f);
    }

}

// The Apple II screen carries NO capture caption. It used to carry two — a
// "Click to capture the mouse" hint and a how-to-get-out reminder — and both
// existed to paper over the click-to-grab contract that is now gone: a click
// that silently changed what the mouse did had to announce itself, and a user
// who got captured by accident had to be told the way out.
//
// Neither problem exists any more. Capture is entered only by Ctrl+Alt+G or a
// middle click, and each of those is also the way out, so anyone captured got
// there deliberately and already knows the gesture. The standing reminder is
// the status-bar GRAB chip (with a long-lived hint beside it); painting over
// the emulated screen to say the same thing is exactly the clutter the chip
// exists to avoid.

void MainWindow::renderKiosk()
{
    // Chrome-free full-viewport window: just the Apple II screen, centred
    // and letterboxed on a black background. No title bar, no resize, no
    // background decoration — the OS window is already full-screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##kiosk", nullptr, flags)) {
        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ─── Kiosk disk selector (gamepad-driven) ───────────────────────────────
//
// A keyboard-free way to flip disks in kiosk mode: Start opens a list of the
// 5.25" images sitting in the same folder as the booted disk (so a game's
// "Side B" is one press away), D-pad/stick move, A mounts the highlighted one
// into the boot Disk II drive (slot 6, drive 1) without rebooting — the
// flip-disk gesture Wings of Fury and friends expect — and B/Start dismiss.

DiskIICard* MainWindow::kioskBootDiskCard()
{
    // Prefer the conventional boot slot 6; fall back to the primary card.
    const auto cards = diskIICards();
    for (auto* card : cards)
        if (card && card->getSlot() == 6) return card;
    return primaryDiskIICard();
}

void MainWindow::openKioskStartMenu()
{
    uiState_->kioskMenuOpen = true;
    kioskPage_     = KioskPage::List;
    kioskZone_     = KioskZone::Games;
    uiState_->kioskActionSelection   = 0;
    kioskRescanDisks();
}

// Rebuild the GAMES list from the booted disk's folder + the extra ROM
// folders. Split from openKioskStartMenu so the RomDirs page can refresh the
// list on its way back — otherwise a folder added/removed there is invisible
// until the menu is closed and reopened.
void MainWindow::kioskRescanDisks()
{
    uiState_->kioskDiskPaths.clear();
    uiState_->kioskDiskLabels.clear();
    uiState_->kioskDiskSelection = 0;
    uiState_->kioskStatus.clear();

    namespace fs = std::filesystem;
    std::error_code ec;

    DiskIICard* boot = kioskBootDiskCard();
    const std::string cur = boot ? boot->getDiskPath(0) : std::string{};

    // Scan the booted disk's own folder PLUS every configured extra ROM
    // folder. Unlike the old build we do NOT filter out unrelated titles:
    // we keep every mountable 5.25" image and SORT by name-proximity so the
    // current title's other sides float to the top while
    // the rest of the collection stays reachable below.
    std::vector<fs::path> dirs;
    auto addDir = [&](const fs::path& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        const fs::path norm = fs::weakly_canonical(d, ec);
        const fs::path key   = ec ? d : norm;
        for (const auto& e : dirs) if (e == key) return;   // dedup
        dirs.push_back(key);
    };
    if (!cur.empty()) addDir(fs::path(cur).parent_path());
    for (const auto& d : uiState_->kioskRomDirectories) addDir(fs::path(d));

    if (dirs.empty()) {
        uiState_->kioskStatus = boot ? "No disk folder to browse — add one via ROM folders"
                            : "No Disk II card in this config";
        return;
    }

    auto toLower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    };
    auto commonPrefix = [](const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        size_t i = 0;
        while (i < n && a[i] == b[i]) ++i;
        return i;
    };
    // A candidate is a "sibling" of the mounted disk when its stem shares a
    // long common prefix (≥6 chars, ≥half the shorter stem) — the same title's
    // other sides ("… (Side A)" ↔ "… (Side B)"), not every disk in the folder.
    const std::string curName = cur.empty() ? std::string{}
                                            : fs::path(cur).filename().string();
    const std::string curKey  = cur.empty() ? std::string{}
                                            : toLower(fs::path(cur).stem().string());
    auto isSibling = [&](const fs::path& p) {
        if (curKey.empty()) return false;
        const std::string k = toLower(p.stem().string());
        if (k == curKey) return true;
        const size_t pref   = commonPrefix(curKey, k);
        const size_t minLen = std::min(curKey.size(), k.size());
        return pref >= 6 && pref * 2 >= minLen;
    };

    // Accept every image the launcher can route — 5.25", 800K 3.5" and HDV —
    // not just floppies. A 5.25" disk is hot-swapped in place (flip-disk); a
    // 3.5"/HDV is mounted + booted through insertAndBootImage on activation.
    std::vector<fs::path> found;
    for (const auto& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (classifyDiskForSlot(it->path().string()) == DiskSlotClass::Unknown)
                continue;
            found.push_back(it->path());
        }
    }
    // Sort: siblings of the mounted disk first, then alphabetical by filename.
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
        const bool sa = isSibling(a), sb = isSibling(b);
        if (sa != sb) return sa;
        return a.filename().string() < b.filename().string();
    });

    // Mark the disk currently in the boot drive so the list shows a ● next to
    // it and the cursor lands on it. Match canonically: the mounted path may
    // be relative (kiosk launched as `POM2 games/foo.dsk`) while scanned
    // entries come out of canonicalized dirs.
    std::error_code ecCur;
    const fs::path curCanon = cur.empty()
        ? fs::path{} : fs::weakly_canonical(fs::path(cur), ecCur);
    uiState_->kioskMountedPath = cur;
    for (const auto& p : found) {
        const std::string name = p.filename().string();
        const bool isMounted = !cur.empty() &&
            (p.string() == cur ||
             (!ecCur && !curCanon.empty() && p == curCanon));
        if (isMounted) {
            uiState_->kioskDiskSelection     = int(uiState_->kioskDiskPaths.size());
            // Adopt the scanned spelling so the render loop's exact string
            // compare against uiState_->kioskDiskPaths draws the ● marker.
            uiState_->kioskMountedPath = p.string();
        }
        uiState_->kioskDiskPaths.push_back(p.string());
        uiState_->kioskDiskLabels.push_back(name);
    }

    if (uiState_->kioskDiskPaths.empty())
        uiState_->kioskStatus = "No disks found in the scanned folder(s)";

    pom2::log().info("Kiosk", "disk scan: " +
                     std::to_string(uiState_->kioskDiskPaths.size()) + " disk(s) across " +
                     std::to_string(dirs.size()) + " folder(s)");
}

void MainWindow::kioskMountSelected()
{
    if (uiState_->kioskDiskSelection < 0 || uiState_->kioskDiskSelection >= int(uiState_->kioskDiskPaths.size())) return;

    const std::string path  = uiState_->kioskDiskPaths[uiState_->kioskDiskSelection];
    const std::string label = uiState_->kioskDiskLabels[uiState_->kioskDiskSelection];

    // 3.5" and HDV volumes are boot media, not swap-in-place floppies: route
    // them into the right card and cold-boot straight away (like the CLI
    // launcher). 5.25" keeps the flip-disk gesture: hot-swap, no reboot.
    if (classifyDiskForSlot(path) != DiskSlotClass::Floppy525) {
        kioskSetPaused(false);          // let the worker run for the boot
        std::string err;
        if (insertAndBootImage(path, err)) {
            uiState_->kioskMountedPath = path;
            uiState_->kioskMenuOpen = false;     // booted → back to the game
        } else {
            uiState_->kioskStatus = "Boot failed: " + err;
        }
        return;
    }

    DiskIICard* boot = kioskBootDiskCard();
    if (!boot) { uiState_->kioskStatus = "No Disk II card in this config"; return; }

    const auto command = storageCoordinator_->mountDiskII(
        *controller, *settings, boot->getSlot(), 0, path);
    if (command.ok) {
        // Keep the menu open so the user can chain a Restart (reboot on the
        // just-mounted disk) without reopening; B / Start dismisses it.
        uiState_->kioskMountedPath = path;
        uiState_->kioskStatus = "Mounted " + label + " — pick RESTART to reboot on it";
    } else {
        uiState_->kioskStatus = "Mount failed: " + command.error;
    }
}

void MainWindow::kioskActivateFocused()
{
    // GAMES zone → mount the highlighted disk in place (no reboot).
    if (kioskZone_ == KioskZone::Games) {
        kioskMountSelected();
        return;
    }

    // ACTIONS zone → 0 Restart · 1 Keyboard · 2 ROM folders ·
    //                3 Exit kiosk · 4 Quit.
    switch (uiState_->kioskActionSelection) {
        case 0: {   // Restart — reboot on whatever disk is now in the drive
            DiskIICard* boot = kioskBootDiskCard();
            kioskSetPaused(false);          // let the worker run for the boot
            if (boot) controller->bootFromSlot(boot->getSlot());
            else      controller->coldBoot();
            controller->setMode(EmulationController::Mode::Running);
            uiState_->kioskMenuOpen = false;
            break;
        }
        case 1:     // Keyboard band — live keys to the running game
            kioskPage_   = KioskPage::Keys;
            uiState_->kioskKeySelection = 0;
            kioskSetPaused(false);          // game keeps running under the band
            break;
        case 2:     // ROM folders manager
            if (kioskPruneRomDirs()) kioskSaveRomDirs();
            uiState_->kioskRomDirectorySelection = 0;
            kioskPage_      = KioskPage::RomDirs;
            break;
        case 3:     // Back to the windowed GUI — machine keeps running
                    // (setKioskModeRuntime closes the menu and un-pauses).
            setKioskModeRuntime(false);
            break;
        case 4:     // Quit — ask for confirmation first
            kioskPage_ = KioskPage::Quit;
            break;
    }
}

// Apple II key grid for the SELECT band. Each cell is an ASCII code the
// running program reads straight from the keyboard latch (queueKey), so no
// make/break bookkeeping is needed (unlike a scancode make/break scheme). Arrows use
// the II's control codes (←=$08 →=$15 ↑=$0B ↓=$0A).
namespace {
struct KioskKey { const char* label; uint8_t ascii; };
const KioskKey kKioskKeys[] = {
    {"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},
    {"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},{"0",'0'},          // row 0 (10)
    {"Q",'Q'},{"W",'W'},{"E",'E'},{"R",'R'},{"T",'T'},
    {"Y",'Y'},{"U",'U'},{"I",'I'},{"O",'O'},{"P",'P'},          // row 1 (10)
    {"A",'A'},{"S",'S'},{"D",'D'},{"F",'F'},{"G",'G'},
    {"H",'H'},{"J",'J'},{"K",'K'},{"L",'L'},                    // row 2 (9)
    {"Z",'Z'},{"X",'X'},{"C",'C'},{"V",'V'},{"B",'B'},
    {"N",'N'},{"M",'M'},                                        // row 3 (7)
    {"SPACE",' '},{"RET",0x0D},{"ESC",0x1B},
    {"\xe2\x86\x90",0x08},{"\xe2\x86\x91",0x0B},
    {"\xe2\x86\x93",0x0A},{"\xe2\x86\x92",0x15},                // row 4 (7)
};
constexpr int kKioskKeyCount = int(sizeof(kKioskKeys) / sizeof(kKioskKeys[0]));
// Row start/end (half-open) indices — mirrors the layout above.
const int kKioskKeyRows[][2] = { {0,10}, {10,20}, {20,29}, {29,36}, {36,43} };
constexpr int kKioskKeyRowN = int(sizeof(kKioskKeyRows) / sizeof(kKioskKeyRows[0]));
}  // namespace

void MainWindow::kioskInjectSelectedKey()
{
    if (uiState_->kioskKeySelection < 0 || uiState_->kioskKeySelection >= kKioskKeyCount) return;
    // The band leaves the machine running, so the latch is read live by the
    // program. queueKey masks to 7 bits and sets the strobe like the hardware.
    controller->memory().queueKey(kKioskKeys[uiState_->kioskKeySelection].ascii);
    uiState_->kioskStatus = std::string("Sent ") + kKioskKeys[uiState_->kioskKeySelection].label;
}

void MainWindow::kioskSetPaused(bool want)
{
    if (want == uiState_->kioskPausedByMenu) return;
    if (want) {
        // Remember whether the machine was ALREADY stopped (user paused it
        // from the GUI toolbar before entering kiosk, or it never started).
        // Without this, closing the menu resumed a machine the user had
        // deliberately paused — the menu's pause is not ours to undo when
        // it was a no-op in the first place.
        uiState_->kioskPauseWasAlreadyStopped =
            controller->getMode() != EmulationController::Mode::Running;
    } else if (uiState_->kioskPauseWasAlreadyStopped) {
        uiState_->kioskPausedByMenu = false;      // give the pause back to the user
        return;
    }
    if (!want) {
        // Resuming: while Stopped, the audio thread kept advancing the
        // speaker's reconstruction cursor over silence, so it now sits far
        // ahead of the (frozen) production. Flush it — otherwise the catch-up
        // logic would swallow the game's first sounds for ~the pause duration.
        controller->speaker().reset();
    }
    controller->setMode(want ? EmulationController::Mode::Stopped
                             : EmulationController::Mode::Running);
    uiState_->kioskPausedByMenu = want;
}

void MainWindow::updateKioskMenu()
{
    // Load the persisted extra ROM folders once (feeds the disk scan below).
    if (!uiState_->kioskRomDirectoriesLoaded) { kioskLoadRomDirs(); uiState_->kioskRomDirectoriesLoaded = true; }

    // The pad was already polled this frame (pollJoystickAndPushToMemory).
    const JoystickInput::UiNav nav = joystick->uiNav();

    // Keyboard fallbacks work even when the controller isn't a recognized
    // GLFW gamepad (so the gamepad-mapped buttons never fire). They mirror
    // the pad: F1/Start opens the Start menu, K/Select the keyboard band,
    // arrows move, Enter validates, Esc goes back.
    //
    // F1, NOT F10: F10 is the global full-screen ⇄ windowed toggle, so
    // using it here meant entering kiosk ALSO opened this menu in the same
    // frame (onKey runs during glfwPollEvents, before render) — the user
    // asked for the game to go full-screen, not for a menu.
    const bool eStart   = nav.menu    || ImGui::IsKeyPressed(ImGuiKey_F1,     false);
    const bool eSelect  = nav.select  || ImGui::IsKeyPressed(ImGuiKey_K,      false);
    const bool eConfirm = nav.confirm || ImGui::IsKeyPressed(ImGuiKey_Enter,  false);
    const bool eCancel  = nav.cancel  || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    // Left/right zone-swap is a one-shot edge (never auto-repeats).
    const bool eLeft    = nav.left    || ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
    const bool eRight   = nav.right   || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);

    // ── SELECT: open/close the keyboard band directly, even mid-game ─────
    // (Back/Select toggles the live keyboard without pausing).
    if (eSelect) {
        if (uiState_->kioskMenuOpen && kioskPage_ == KioskPage::Keys) {
            uiState_->kioskMenuOpen = false;
        } else {
            uiState_->kioskMenuOpen = true;
            kioskPage_     = KioskPage::Keys;
            uiState_->kioskKeySelection   = 0;
            uiState_->kioskStatus.clear();
        }
    }

    // ── START: open/close the Start menu ────────────────────────────────
    if (eStart) {
        if (uiState_->kioskMenuOpen && kioskPage_ != KioskPage::Keys) uiState_->kioskMenuOpen = false;
        else openKioskStartMenu();
    }

    // Pause the machine on every Start-menu page, but NOT the keyboard band
    // (its keys must reach a running game). Closed menu → running.
    const bool wantPause = uiState_->kioskMenuOpen && kioskPage_ != KioskPage::Keys;
    kioskSetPaused(wantPause);
    // Re-park if something else resumed the worker behind the open menu
    // (e.g. an F6 hold released across the menu-open frame ends in
    // rewindEndAndResume → Mode::Running); kioskSetPaused alone early-outs
    // because uiState_->kioskPausedByMenu still says "paused".
    if (wantPause && controller->getMode() != EmulationController::Mode::Stopped)
        controller->setMode(EmulationController::Mode::Stopped);

    if (!uiState_->kioskMenuOpen) return;

    // ── Temporal auto-repeat for held directions (400ms delay, 150ms rate) ──
    // The paused loop runs unthrottled, so a per-frame step would be
    // unaimable; gate held-direction steps on the wall clock instead. Edge
    // presses from the keyboard already single-step via IsKeyPressed below,
    // so `step` covers the *held* pad/keys case only.
    const bool upHeld   = nav.upHeld   || ImGui::IsKeyDown(ImGuiKey_UpArrow);
    const bool downHeld = nav.downHeld || ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool leftHeld = nav.leftHeld || ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const bool rightHeld= nav.rightHeld|| ImGui::IsKeyDown(ImGuiKey_RightArrow);
    const bool pgUpHeld = nav.pageUpHeld  || ImGui::IsKeyDown(ImGuiKey_PageUp);
    const bool pgDnHeld = nav.pageDownHeld|| ImGui::IsKeyDown(ImGuiKey_PageDown);
    const bool navHeld  = upHeld || downHeld || leftHeld || rightHeld ||
                          pgUpHeld || pgDnHeld;
    const double tNow = ImGui::GetTime();
    bool step = false;
    if (navHeld) {
        if (!uiState_->kioskNavigationHeld) { step = true; uiState_->kioskNavigationHeld = true; uiState_->kioskNavigationNextTime = tNow + 0.40; }
        else if (tNow >= uiState_->kioskNavigationNextTime) { step = true; uiState_->kioskNavigationNextTime = tNow + 0.15; }
    } else {
        uiState_->kioskNavigationHeld = false;
    }
    // Resolve directional intents: a fresh keyboard edge OR a repeat `step`.
    const bool up    = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   false) || (step && upHeld);
    const bool down  = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || (step && downHeld);
    const bool left  = step && leftHeld;   // (edge handled separately below)
    const bool right = step && rightHeld;
    const bool pgUp  = ImGui::IsKeyPressed(ImGuiKey_PageUp,   false) || (step && pgUpHeld);
    const bool pgDn  = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || (step && pgDnHeld);

    switch (kioskPage_) {

    case KioskPage::Quit:
        if (eConfirm) { if (window) glfwSetWindowShouldClose(window, 1); uiState_->kioskMenuOpen = false; }
        if (eCancel)  kioskPage_ = KioskPage::List;
        break;

    case KioskPage::Keys: {
        // 2D grid: up/down change row (clamp column), left/right within row.
        if (up || down) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (uiState_->kioskKeySelection >= kKioskKeyRows[r][0] && uiState_->kioskKeySelection < kKioskKeyRows[r][1]) row = r;
            int col = uiState_->kioskKeySelection - kKioskKeyRows[row][0];
            row = (row + (down ? 1 : -1) + kKioskKeyRowN) % kKioskKeyRowN;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            if (col >= len) col = len - 1;
            uiState_->kioskKeySelection = kKioskKeyRows[row][0] + col;
        }
        if (left || right || eLeft || eRight) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (uiState_->kioskKeySelection >= kKioskKeyRows[r][0] && uiState_->kioskKeySelection < kKioskKeyRows[r][1]) row = r;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            int col = uiState_->kioskKeySelection - kKioskKeyRows[row][0] + ((right || eRight) ? 1 : -1);
            col = std::max(0, std::min(len - 1, col));
            uiState_->kioskKeySelection = kKioskKeyRows[row][0] + col;
        }
        if (eConfirm) kioskInjectSelectedKey();   // send key, band stays open
        if (eCancel)  uiState_->kioskMenuOpen = false;      // (B) close → resume game
        break;
    }

    case KioskPage::RomDirs: {
        const int total = 1 + int(uiState_->kioskRomDirectories.size());   // [0]=ADD, [1..]=folders
        if (up || down) {
            uiState_->kioskRomDirectorySelection += down ? 1 : -1;
            uiState_->kioskRomDirectorySelection = (uiState_->kioskRomDirectorySelection % total + total) % total;
        }
        if (eConfirm) {
            if (uiState_->kioskRomDirectorySelection == 0) {                     // + ADD → browser
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path start = (!uiState_->kioskRomDirectories.empty() &&
                                  fs::is_directory(uiState_->kioskRomDirectories.back(), ec))
                                     ? fs::path(uiState_->kioskRomDirectories.back())
                                     : fs::current_path(ec);
                fs::path abs = fs::absolute(start, ec);
                uiState_->kioskBrowseDirectory = (ec ? start : abs).lexically_normal().string();
                kioskComputeShortcuts();
                kioskScanBrowse(uiState_->kioskBrowseDirectory);
                kioskPage_ = KioskPage::Browse;
            } else {                                        // remove this folder
                const int idx = uiState_->kioskRomDirectorySelection - 1;
                if (idx >= 0 && idx < int(uiState_->kioskRomDirectories.size()))
                    uiState_->kioskRomDirectories.erase(uiState_->kioskRomDirectories.begin() + idx);
                kioskSaveRomDirs();
                if (uiState_->kioskRomDirectorySelection > int(uiState_->kioskRomDirectories.size()))
                    uiState_->kioskRomDirectorySelection = int(uiState_->kioskRomDirectories.size());
            }
        }
        if (eCancel) {
            kioskPage_ = KioskPage::List;
            kioskZone_ = KioskZone::Actions;
            kioskRescanDisks();   // pick up folders added/removed just now
        }
        break;
    }

    case KioskPage::Browse: {
        namespace fs = std::filesystem;
        const int nShort = int(uiState_->kioskBrowseShortcutPaths.size());
        const int total  = 2 + nShort + int(uiState_->kioskBrowseSubdirectories.size());
        if (up || down) {
            uiState_->kioskBrowseSelection += down ? 1 : -1;
            uiState_->kioskBrowseSelection = (uiState_->kioskBrowseSelection % total + total) % total;
        }
        if (eConfirm) {
            if (uiState_->kioskBrowseSelection == 0) {                     // USE THIS FOLDER
                if (std::find(uiState_->kioskRomDirectories.begin(), uiState_->kioskRomDirectories.end(), uiState_->kioskBrowseDirectory)
                        == uiState_->kioskRomDirectories.end())
                    uiState_->kioskRomDirectories.push_back(uiState_->kioskBrowseDirectory);
                kioskSaveRomDirs();
                uiState_->kioskRomDirectorySelection = 0;
                kioskPage_ = KioskPage::RomDirs;
            } else if (uiState_->kioskBrowseSelection == 1) {              // .. parent
                const fs::path p(uiState_->kioskBrowseDirectory);
                if (p.has_parent_path() && p.parent_path() != p)
                    uiState_->kioskBrowseDirectory = p.parent_path().string();
                kioskScanBrowse(uiState_->kioskBrowseDirectory);
                uiState_->kioskBrowseSelection = 0;
            } else if (uiState_->kioskBrowseSelection < 2 + nShort) {      // shortcut
                uiState_->kioskBrowseDirectory = uiState_->kioskBrowseShortcutPaths[uiState_->kioskBrowseSelection - 2];
                kioskScanBrowse(uiState_->kioskBrowseDirectory);
                uiState_->kioskBrowseSelection = 0;
            } else {                                        // descend
                uiState_->kioskBrowseDirectory = uiState_->kioskBrowseSubdirectories[uiState_->kioskBrowseSelection - 2 - nShort];
                kioskScanBrowse(uiState_->kioskBrowseDirectory);
                uiState_->kioskBrowseSelection = 0;
            }
        }
        if (eCancel) kioskPage_ = KioskPage::RomDirs;
        break;
    }

    case KioskPage::List: default: {
        const int nd = int(uiState_->kioskDiskPaths.size());
        // LEFT/RIGHT (edge) swaps focus between the GAMES and ACTIONS zones.
        if (eLeft || eRight)
            kioskZone_ = (kioskZone_ == KioskZone::Games) ? KioskZone::Actions
                                                          : KioskZone::Games;
        if (kioskZone_ == KioskZone::Games) {
            if (nd > 0) {
                constexpr int kPage = 10;   // L1/R1 fast jump size
                int delta = 0;
                if      (down) delta =  1;
                else if (up)   delta = -1;
                else if (pgDn) delta =  kPage;
                else if (pgUp) delta = -kPage;
                if (delta != 0) {
                    uiState_->kioskDiskSelection += delta;
                    if (delta == 1 || delta == -1)          // step: wrap
                        uiState_->kioskDiskSelection = (uiState_->kioskDiskSelection % nd + nd) % nd;
                    else                                    // page jump: clamp
                        uiState_->kioskDiskSelection = std::max(0, std::min(nd - 1, uiState_->kioskDiskSelection));
                }
            }
        } else if (up || down) {                            // ACTIONS column
            uiState_->kioskActionSelection += down ? 1 : -1;
            uiState_->kioskActionSelection = (uiState_->kioskActionSelection % kKioskActionCount + kKioskActionCount)
                           % kKioskActionCount;
        }
        if (eConfirm) kioskActivateFocused();
        if (eCancel)  uiState_->kioskMenuOpen = false;               // (B) resume game
        break;
    }
    }

    // A close on any page must let the machine run again.
    if (!uiState_->kioskMenuOpen) kioskSetPaused(false);
}

void MainWindow::renderKioskMenu()
{
    if (!uiState_->kioskMenuOpen) return;
    namespace fs = std::filesystem;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 disp = vp->Size;
    const bool keysPage = (kioskPage_ == KioskPage::Keys);

    const ImVec4 kYellow(1.0f, 0.85f, 0.30f, 1.0f);
    const ImVec4 kGreen (0.47f, 0.90f, 0.47f, 1.0f);
    const ImVec4 kDim   (0.50f, 0.50f, 0.50f, 1.0f);
    const ImVec4 kGrey  (0.60f, 0.60f, 0.60f, 1.0f);

    // Full-screen dim veil behind every page EXCEPT the keyboard band (there
    // the game must stay visible so you see keys land).
    if (!keysPage) {
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(disp);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##kioskVeil", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();
    }

    // Geometry: centered panel for full-screen pages, bottom band for keys.
    if (keysPage) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.98f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.66f, disp.y * 0.34f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.82f, disp.y * 0.80f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, 245));
    }
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(220, 200, 80, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    // The kiosk screen is a full-viewport OPAQUE window — force the menu to
    // the front every frame or it renders hidden behind the black background.
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavInputs;

    ImGui::Begin("##kioskMenu", nullptr, flags);

    auto rowText = [&](bool sel, const ImVec4& col, const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[512]; std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s %s", sel ? "\xe2\x96\xb6" : "  ", buf);
        if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
    };

    // ================= LIST — two zones (games / actions) ================
    if (kioskPage_ == KioskPage::List) {
        const int nd = int(uiState_->kioskDiskLabels.size());
        const bool zGames = (kioskZone_ == KioskZone::Games);

        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(kYellow, ICON_FA_GAMEPAD " MENU");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "\xe2\x97\x80\xe2\x96\xb6 switch zone   \xc2\xb7   "
                                  "up/down select   \xc2\xb7   L1/R1 fast   \xc2\xb7   "
                                  "A confirm   \xc2\xb7   B resume   \xc2\xb7   SELECT keyboard");
        ImGui::Separator();

        // Footer reserve = the 4 action rows @2.3 + a "disks found" line @1.3.
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float bf = ImGui::GetFontSize();
        // 5.0f = the five action rows @2.3 (Restart / Keyboard / ROM
        // folders / Exit kiosk / Quit). Reserving four clipped the last
        // one below the panel edge — the window is NoScrollbar and a
        // gamepad user cannot scroll to it.
        const float footer = 5.0f * (bf * 2.3f + sp) + (bf * 1.3f + sp) + (sp + 6.0f);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextColored(zGames ? kYellow : kDim,
                           zGames ? "\xe2\x96\xb6 " ICON_FA_COMPACT_DISC " GAMES"
                                  : "  " ICON_FA_COMPACT_DISC " GAMES");
        ImGui::BeginChild("##kdList", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), true);
        ImGui::SetWindowFontScale(2.6f);
        if (nd == 0)
            ImGui::TextColored(kDim, "(no disks — add a ROM folder)");
        for (int i = 0; i < nd; ++i) {
            const bool sel = (i == uiState_->kioskDiskSelection);
            const ImVec4 col = zGames ? kGreen : kDim;
            // ● marks the disk currently in the boot drive (flip-disk anchor).
            const bool mounted = !uiState_->kioskMountedPath.empty() &&
                                 uiState_->kioskDiskPaths[i] == uiState_->kioskMountedPath;
            rowText(sel, col, "%s%s", mounted ? ICON_FA_COMPACT_DISC " " : "",
                    uiState_->kioskDiskLabels[i].c_str());
        }
        ImGui::EndChild();

        // Actions column.
        ImGui::SetWindowFontScale(2.3f);
        const bool zAct = (kioskZone_ == KioskZone::Actions);
        auto actionRow = [&](int idx, const ImVec4& col, const char* label) {
            const bool sel = (uiState_->kioskActionSelection == idx);
            ImGui::PushStyleColor(ImGuiCol_Text, (sel && zAct) ? kGreen : (zAct ? col : kDim));
            ImGui::Text("%s %s", (sel && zAct) ? "\xe2\x96\xb6" : "  ", label);
            ImGui::PopStyleColor();
        };
        actionRow(0, ImVec4(1.0f, 0.60f, 0.15f, 1.0f), ICON_FA_ROTATE " RESTART MACHINE");
        actionRow(1, ImVec4(0.55f, 0.80f, 1.0f, 1.0f), ICON_FA_KEYBOARD " KEYBOARD");
        actionRow(2, ImVec4(0.60f, 0.95f, 0.60f, 1.0f), ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        // Exit to the windowed GUI. Discoverable here because a kiosk user
        // has no menu bar and may not know about Ctrl+Alt+F / F10.
        actionRow(3, ImVec4(0.80f, 0.80f, 1.0f, 1.0f),
                  ICON_FA_COMPRESS " EXIT KIOSK (WINDOWED)");
        actionRow(4, ImVec4(1.0f, 0.50f, 0.40f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT");

        ImGui::SetWindowFontScale(1.3f);
        ImGui::Separator();
        ImGui::TextColored(kYellow, ICON_FA_COMPACT_DISC " Disks found: %d", nd);
        if (!uiState_->kioskStatus.empty()) ImGui::TextColored(kGrey, "%s", uiState_->kioskStatus.c_str());
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= KEYBOARD band (game runs underneath) ==============
    else if (kioskPage_ == KioskPage::Keys) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(kYellow, ICON_FA_KEYBOARD " KEYBOARD");
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "  A send  \xc2\xb7  B close  \xc2\xb7  game keeps running");
        ImGui::Separator();
        for (int r = 0; r < kKioskKeyRowN; ++r) {
            ImGui::SetWindowFontScale(2.2f);
            for (int i = kKioskKeyRows[r][0]; i < kKioskKeyRows[r][1]; ++i) {
                const bool sel = (i == uiState_->kioskKeySelection);
                if (i > kKioskKeyRows[r][0]) ImGui::SameLine();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                char cell[24];
                std::snprintf(cell, sizeof cell, sel ? "[%s]" : " %s ", kKioskKeys[i].label);
                ImGui::TextUnformatted(cell);
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= QUIT confirmation =================================
    else if (kioskPage_ == KioskPage::Quit) {
        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT?");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 30));
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(kGreen, ICON_FA_POWER_OFF " (A) Yes, quit");
        ImGui::SetWindowFontScale(1.8f);
        ImGui::TextColored(kGrey, "(B) No, back to menu");
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= ROM folders manager ==============================
    else if (kioskPage_ == KioskPage::RomDirs) {
        ImGui::SetWindowFontScale(2.6f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A add / remove   \xc2\xb7   B back");
        ImGui::Separator();
        ImGui::BeginChild("##kRomDirs", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.2f);
        const int total = 1 + int(uiState_->kioskRomDirectories.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == uiState_->kioskRomDirectorySelection);
            if (i == 0) {
                rowText(sel, kGreen, ICON_FA_PLUS " [ ADD A FOLDER ]");
            } else {
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%s " ICON_FA_XMARK " %s", sel ? "\xe2\x96\xb6" : "  ",
                            uiState_->kioskRomDirectories[i - 1].c_str());
                if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
            }
        }
        if (uiState_->kioskRomDirectories.empty()) {
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(kDim, "   (only the booted disk's folder is scanned)");
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= Directory browser ================================
    else if (kioskPage_ == KioskPage::Browse) {
        ImGui::SetWindowFontScale(2.4f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " SELECT ROM FOLDER");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A enter / select   \xc2\xb7   B cancel");
        ImGui::Separator();
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", uiState_->kioskBrowseDirectory.c_str());
        ImGui::Separator();
        ImGui::BeginChild("##kBrowse", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.1f);
        const int nShort = int(uiState_->kioskBrowseShortcutPaths.size());
        const int total  = 2 + nShort + int(uiState_->kioskBrowseSubdirectories.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == uiState_->kioskBrowseSelection);
            if (i == 0)                 rowText(sel, kGreen, ICON_FA_STAR " [ USE THIS FOLDER ]");
            else if (i == 1)            rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " ..");
            else if (i < 2 + nShort)    rowText(sel, kGreen, "%s", uiState_->kioskBrowseShortcutLabels[i - 2].c_str());
            else                        rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " %s",
                                                fs::path(uiState_->kioskBrowseSubdirectories[i - 2 - nShort]).filename().string().c_str());
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(keysPage ? 1 : 2);
}

// ─── Kiosk ROM-folders manager + directory browser ──────────────────────

void MainWindow::kioskScanBrowse(const std::string& dir)
{
    namespace fs = std::filesystem;
    uiState_->kioskBrowseSubdirectories.clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) uiState_->kioskBrowseSubdirectories.push_back(it->path().string());
    }
    std::sort(uiState_->kioskBrowseSubdirectories.begin(), uiState_->kioskBrowseSubdirectories.end(),
              [](const std::string& a, const std::string& b) {
                  return fs::path(a).filename().string() < fs::path(b).filename().string();
              });
    uiState_->kioskBrowseSelection = 0;
}

void MainWindow::kioskComputeShortcuts()
{
    namespace fs = std::filesystem;
    uiState_->kioskBrowseShortcutPaths.clear();
    uiState_->kioskBrowseShortcutLabels.clear();
    auto add = [&](const std::string& path, const std::string& label) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return;
        if (std::find(uiState_->kioskBrowseShortcutPaths.begin(), uiState_->kioskBrowseShortcutPaths.end(),
                      path) != uiState_->kioskBrowseShortcutPaths.end()) return;   // dedup
        uiState_->kioskBrowseShortcutPaths.push_back(path);
        uiState_->kioskBrowseShortcutLabels.push_back(label);
    };
    add("/", std::string(ICON_FA_SERVER) + " / (filesystem root)");
    if (const char* home = std::getenv("HOME"))
        add(home, std::string(ICON_FA_FOLDER_OPEN) + " Home");
    // Removable-media mount points (guarded so each OS only shows what exists).
    const char* user = std::getenv("USER");
    std::vector<std::string> roots{ "/Volumes" };
    if (user) { roots.push_back(std::string("/run/media/") + user);
                roots.push_back(std::string("/media/") + user); }
    roots.push_back("/mnt");
    for (const auto& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2))
                add(it->path().string(),
                    std::string(ICON_FA_HARD_DRIVE) + " " + it->path().filename().string());
        }
    }
}

// Extra ROM folders persist OUTSIDE state.cfg (kiosk keeps its main config
// read-only), in a sibling "uiState_->kioskromdirs.txt" — one absolute path per line.
static std::filesystem::path kioskRomDirsFile(const pom2::Settings& s)
{
    namespace fs = std::filesystem;
    fs::path store = s.getStorePath();
    fs::path dir = store.has_parent_path() ? store.parent_path() : fs::path(".");
    return dir / "uiState_->kioskromdirs.txt";
}

void MainWindow::kioskLoadRomDirs()
{
    namespace fs = std::filesystem;
    uiState_->kioskRomDirectories.clear();
    std::ifstream f(kioskRomDirsFile(*settings));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::error_code ec;
        if (!line.empty() && fs::is_directory(line, ec)) uiState_->kioskRomDirectories.push_back(line);
    }
}

void MainWindow::kioskSaveRomDirs()
{
    std::ofstream f(kioskRomDirsFile(*settings), std::ios::trunc);
    if (!f) return;
    for (const auto& d : uiState_->kioskRomDirectories) f << d << '\n';
}

bool MainWindow::kioskPruneRomDirs()
{
    namespace fs = std::filesystem;
    bool changed = false;
    for (size_t i = 0; i < uiState_->kioskRomDirectories.size(); ) {
        std::error_code ec;
        if (!fs::is_directory(uiState_->kioskRomDirectories[i], ec)) {
            uiState_->kioskRomDirectories.erase(uiState_->kioskRomDirectories.begin() + long(i));
            changed = true;
        } else ++i;
    }
    return changed;
}

// ─── Mouse Card input routing ───────────────────────────────────────────

// `MouseGrab.h` stays GLFW-free so a headless test can link it. Prove its
// mirrored tokens still match the real ones here, where <GLFW/glfw3.h> is
// in scope — an upstream renumbering becomes a compile error, not a chord
// that silently stops working.
static_assert(pom2::mousegrab::kKeyG         == GLFW_KEY_G);
static_assert(pom2::mousegrab::kModControl   == GLFW_MOD_CONTROL);
static_assert(pom2::mousegrab::kModAlt       == GLFW_MOD_ALT);
static_assert(pom2::mousegrab::kButtonLeft   == GLFW_MOUSE_BUTTON_LEFT);
static_assert(pom2::mousegrab::kButtonMiddle == GLFW_MOUSE_BUTTON_MIDDLE);

pom2::mousegrab::Context MainWindow::mouseGrabContext() const
{
    pom2::mousegrab::Context c;
    c.cardPlugged = mouseCoordinator_->capture().plugged();
    c.grabbed     = uiState_->mouseGrabbed;
    c.voxelView   = uiState_->show3dVoxel;
    // Hover, NOT rect containment. `uiState_->screenHovered` is ImGui's own z-order
    // aware verdict, captured next to the screen Image (renderScreenWindow).
    // A raw "is the cursor between uiState_->screenRectMin and uiState_->screenRectMax" test
    // cannot see what is drawn on top: an open dropdown, a popup or a panel
    // docked over the screen all sit *inside* that rect, so every click the
    // user aimed at the menu also reached the Mouse Card — and, worse, armed
    // `shouldGrabOnPress` into capturing the pointer behind the menu.
    // The rect itself is still the right tool for the *coordinate* mapping
    // in onMouseMove; it is only wrong as an ownership test.
    c.screenHovered = uiState_->screenHovered;
    return c;
}

bool MainWindow::mouseGrabbed() const
{
    return uiState_->mouseGrabbed;
}

void MainWindow::setMouseGrab(bool on)
{
    if (on == uiState_->mouseGrabbed) return;
    if (on && !mouseCoordinator_->capture().plugged()) {
        // Capturing the pointer with nothing to feed would strand the user
        // in a hidden-cursor mode for no gain.
        uiState_->tapeStatusMessage = "Mouse capture: no Mouse Card plugged "
                            "(Slot Configuration → mouse / mouseaw)";
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        return;
    }
    uiState_->mouseGrabbed = on;

    if (window) {
        // GLFW_CURSOR_DISABLED hides the OS cursor AND unbounds it: the
        // reported position keeps accumulating past the window edges, which
        // is exactly the infinite-delta source a relative quadrature mouse
        // wants. GLFW restores the pre-grab cursor position on the way out.
        glfwSetInputMode(window, GLFW_CURSOR,
                         on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#ifndef __EMSCRIPTEN__
        // Raw (unaccelerated, unscaled) motion while captured — the desktop's
        // pointer-acceleration curve is tuned for a screen-sized target area,
        // and it makes the guest cursor's speed depend on how fast the user
        // flicks. Only meaningful under GLFW_CURSOR_DISABLED. The browser has
        // no equivalent knob (pointer lock already delivers raw movementX/Y).
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                             on ? GLFW_TRUE : GLFW_FALSE);
        }
#endif
    }

    // Take the mouse away from ImGui for the duration: under a captured
    // pointer io.MousePos tracks the virtual cursor, which would hover and
    // click panels the user cannot see. The GLFW backend already skips its
    // own cursor-shape updates while GLFW_CURSOR_DISABLED is set
    // (imgui_impl_glfw.cpp `ImGui_ImplGlfw_UpdateMouseCursor`), so the two
    // do not fight over the input mode.
    ImGuiIO& io = ImGui::GetIO();
    if (on) io.ConfigFlags |=  ImGuiConfigFlags_NoMouse;
    else    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    // Both edges warp the reported cursor position (into the virtual space on
    // entry, back to the real one on exit). Re-seed the delta baseline so the
    // first event after the transition doesn't inject that jump as motion,
    // and drop sub-pixel residue accumulated under the other regime.
    uiState_->mouseInitialized    = false;
    uiState_->mouseSubAppleX = 0.0;
    uiState_->mouseSubAppleY = 0.0;

    // Never leave the guest holding a button it can no longer release.
    if (!on && uiState_->mouseButtonHeld) {
        uiState_->mouseButtonHeld = false;
        (void)mouseCoordinator_->routeHost(
            uiState_->mouseAppleX, uiState_->mouseAppleY, false);
    }

    uiState_->tapeStatusMessage = on
        ? "Mouse captured — Ctrl+Alt+G or middle click to release"
        : "Mouse released";
    uiState_->tapeStatusUntil     = uiState_->lastFrameTime + (on ? 4.0 : 2.0);
    // The bar-side "how to get out" hint. 4 s was tuned for a caption
    // painted over the emulated screen, where it had to get out of the way
    // fast; in the status bar it costs only bar width, and the person who
    // needs it is the one still working out where their pointer went. Long
    // enough to notice, read and act on without it becoming furniture.
    uiState_->mouseGrabHintUntil = on ? uiState_->lastFrameTime + 30.0 : 0.0;
}

void MainWindow::toggleMouseGrab() { setMouseGrab(!uiState_->mouseGrabbed); }

void MainWindow::onWindowFocus(bool focused)
{
    if (!focused) setMouseGrab(false);
}

void MainWindow::onMouseMove(double x, double y)
{
    // First call after startup just seeds last-position; no delta yet.
    if (!uiState_->mouseInitialized) {
        uiState_->lastMouseHostX = x;
        uiState_->lastMouseHostY = y;
        uiState_->mouseInitialized = true;
        return;
    }
    const double rawDx = x - uiState_->lastMouseHostX;
    const double rawDy = y - uiState_->lastMouseHostY;
    uiState_->lastMouseHostX = x;
    uiState_->lastMouseHostY = y;

    // Either MAME-faithful MouseCard or AppleWin HLE MouseCardAppleWin
    // can be plugged (mutually exclusive). Both expose the same
    // `setHostMouse(rawX, rawY, button)` + `getSlot()` API; route through
    // tiny lambdas so the absolute / relative cursor logic below stays
    // variant-agnostic.
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();
    // Card plugged + (pointer captured, or hovering the screen widget).
    // Uncaptured motion outside the widget belongs to ImGui — see MouseGrab.h.
    if (!pom2::mousegrab::shouldRouteMotion(grabCtx)) return;
    auto pushMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        (void)mouseCoordinator_->routeHost(rx, ry, btn);
    };
    // Need a valid Apple II Screen widget rect to map host pixels into
    // Apple-cursor coordinates. Bail until renderScreen has populated it.
    const float widgetW = uiState_->screenRectMax.x - uiState_->screenRectMin.x;
    const float widgetH = uiState_->screenRectMax.y - uiState_->screenRectMin.y;
    if (widgetW <= 0.0f || widgetH <= 0.0f) return;

    // ── Absolute closed-loop cursor sync (AppleWin HLE only) ───────────
    // When the `mouseaw` card is plugged AND the AppleMouse firmware has
    // been turned on (MODE_MOUSE_ON, bit 0 of the latched MODE byte), the
    // card's HLE'd MCU keeps the cursor position in `iX/iY` clamped to
    // the firmware-installed window `[iMinX..iMaxX] × [iMinY..iMaxY]`.
    // We read that authoritative state via the debug snapshot, project
    // the host cursor's position onto the widget rect (saturating clamp
    // outside, so wandering out of the widget pins the Apple cursor at
    // the matching edge instead of letting it drift), and inject the
    // delta needed to drive `iX/iY` toward the projected target. The
    // earlier closed-loop attempt (reverted in commit ccd9a95) failed
    // because it assumed the clamp range equalled the display resolution
    // — that's true for the //e desktop but wrong for e.g. MousePaint's
    // 0..559 horizontal clamp. Using the card-reported clamp window
    // sidesteps that guess entirely.
    // Each push is bounded to ±127 (the MCU's 8-bit signed wrap range);
    // large gaps (first event after re-entry, big window resize) converge
    // over several events.
    bool absoluteHandled = false;
    const auto mouse = mouseCoordinator_->capture();
    if (mouse.appleWinActive() &&
        pom2::mousegrab::allowAbsoluteSync(grabCtx)) {
        const auto& s = mouse.appleWin;
        const bool mouseOn = s.mouseOn();
        const int rangeX = s.iMaxX - s.iMinX;
        const int rangeY = s.iMaxY - s.iMinY;
        if (mouseOn && rangeX > 0 && rangeY > 0) {
            const double fracX = std::clamp(
                (x - double(uiState_->screenRectMin.x)) / double(widgetW), 0.0, 1.0);
            const double fracY = std::clamp(
                (y - double(uiState_->screenRectMin.y)) / double(widgetH), 0.0, 1.0);
            const int targetX = s.iMinX + int(fracX * rangeX + 0.5);
            const int targetY = s.iMinY + int(fracY * rangeY + 0.5);
            int dx = targetX - s.iX;
            int dy = targetY - s.iY;
            if (dx >  127) dx =  127;
            if (dx < -127) dx = -127;
            if (dy >  127) dy =  127;
            if (dy < -127) dy = -127;
            uiState_->mouseAppleX = static_cast<uint8_t>(uiState_->mouseAppleX + dx);
            uiState_->mouseAppleY = static_cast<uint8_t>(uiState_->mouseAppleY + dy);
            pushMouse(uiState_->mouseAppleX, uiState_->mouseAppleY, uiState_->mouseButtonHeld);
            // Drop relative sub-pixel residue so a later fallback (mouse
            // turned off mid-session) doesn't replay stale fractional
            // motion accumulated before sync was active.
            uiState_->mouseSubAppleX = 0.0;
            uiState_->mouseSubAppleY = 0.0;
            absoluteHandled = true;
        }
    }
    if (absoluteHandled) return;

    // ── Relative drive (fallback, and the only path while captured) ──
    // Used by the MAME-faithful MouseCard (no iX/iY exposed — firmware
    // lives inside the 68705P3 MCU's internal RAM), by the AppleWin HLE
    // card before the firmware enables MOUSE_ON, and by BOTH whenever the
    // pointer is captured (a grabbed cursor has no meaningful position in
    // the widget, only deltas — see MouseGrab.h). The cursor-inside-widget
    // gate was already applied by `shouldRouteMotion` above.

    // ── Speed mapping (relative drive — the only path) ──────────────
    // The closed-loop absolute sync was an experiment that didn't survive
    // contact with real apps: the cursor's real clamp range lives behind the
    // firmware (the MCU on the //e card, the internal ROM on the //c) and the
    // app's ClampMouse parameters don't reliably land in 6502-readable holes
    // for MGTK-based apps (A2Desktop/MousePaint), so any absolute target was
    // guesswork. The proven proportional drive below — what AppleWin/MAME do
    // — gives no centre-jump and lets the app's own firmware clamp at its
    // own edges naturally.
    // Used when the AppleMouse firmware is off or its clamp window is
    // non-standard (holes out of display range). Convert host-pixel
    // deltas to Apple-cursor units so 1 host pixel of motion = 1 host
    // pixel of cursor motion visually in the widget.
    //   apple_per_host_px = logical_screen_dim / widget_host_dim
    // The widget is ALWAYS drawn at kWidth(280)×kHeight(192) aspect
    // (drawScreenImage), so the X mapping must use kWidth, NOT
    // display->width() — the latter returns 560 in DHGR/80-col, which made
    // X track 2× faster than Y in 80-column mode (where A2Desktop runs).
    // Both axes now share the same logical→widget scale. Sub-pixel motion
    // accumulates across events.
    const double sxRatio = double(Apple2Display::kWidth)  / double(widgetW);
    const double syRatio = double(Apple2Display::kHeight) / double(widgetH);
    uiState_->mouseSubAppleX += rawDx * sxRatio;
    uiState_->mouseSubAppleY += rawDy * syRatio;
    int dxApple = static_cast<int>(uiState_->mouseSubAppleX);
    int dyApple = static_cast<int>(uiState_->mouseSubAppleY);
    // Clamp BEFORE consuming the sub-pixel accumulator so big jumps
    // (>127 ticks in one event, e.g. cursor teleported across widget)
    // carry the residual forward to the next event instead of being
    // silently dropped. ±127 = MCU's 8-bit signed wrap-correction range.
    if (dxApple >  127) dxApple =  127;
    if (dxApple < -127) dxApple = -127;
    if (dyApple >  127) dyApple =  127;
    if (dyApple < -127) dyApple = -127;
    uiState_->mouseSubAppleX -= dxApple;
    uiState_->mouseSubAppleY -= dyApple;

    uiState_->mouseAppleX = static_cast<uint8_t>(uiState_->mouseAppleX + dxApple);
    uiState_->mouseAppleY = static_cast<uint8_t>(uiState_->mouseAppleY + dyApple);
    pushMouse(uiState_->mouseAppleX, uiState_->mouseAppleY, uiState_->mouseButtonHeld);
}

void MainWindow::onMouseButton(int button, int action)
{
    const bool press = (action != 0);   // GLFW_RELEASE = 0, others = press/repeat
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();

    // Middle click TOGGLES the capture, matching what every VM viewer trains
    // into the user's fingers, and it is one of exactly two gestures that
    // can — Ctrl+Alt+G is the other. Checked first and on PRESS only:
    // releasing the wheel button must not toggle back, and while captured
    // ImGui has no mouse, so nothing else wants this event.
    //
    // A left press does NOT capture. That contract is gone on purpose: it
    // made an ordinary click silently change the meaning of every later
    // click, and the capturing press had to be swallowed (the guest cursor
    // is wherever its firmware left it, not under the host pointer), so the
    // user's first click simply vanished. Left presses now always route by
    // shouldRouteButton below and mean what they look like.
    if (pom2::mousegrab::isToggleButton(button)) {
        if (press && pom2::mousegrab::shouldToggleGrab(grabCtx))
            setMouseGrab(!uiState_->mouseGrabbed);
        return;
    }

    // Only the primary button is wired to the Apple Mouse Card (PB7 of the
    // MCU). Captured, every press is the guest's; uncaptured, only presses
    // over the Apple II Screen widget are (the rest belong to ImGui —
    // menus, panels). A RELEASE always passes through, so a button pressed
    // inside the screen but released outside still gets cleared on the card.
    if (!pom2::mousegrab::shouldRouteButton(grabCtx, button, press)) return;

    uiState_->mouseButtonHeld = press;
    (void)mouseCoordinator_->routeHost(
        uiState_->mouseAppleX, uiState_->mouseAppleY,
        uiState_->mouseButtonHeld);
}

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        uiState_->tapeStatusMessage = "HDV boot failed: no image loaded";
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        return;
    }
    const std::string p = dev->getImagePath();
    // Boot from the slot the HDV/CFFA card is actually plugged in — the user
    // can move it to slot 2 / 7 / etc. via Slot Configuration and the
    // boot path follows. The card's slot ROM bakes its slot number into
    // the ProDOS dispatcher trampolines, so `bootFromSlot(N)` lands on
    // the right $C(N)00 entry point automatically.
    controller->bootFromSlot(dev->getSlot());
    uiState_->tapeStatusMessage = "Booting HDV (slot " +
        std::to_string(dev->getSlot()) + "): " + p;
    uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
}

void MainWindow::renderPasteFileDialog()
{
    if (uiState_->showPasteFileDialog) {
        ImGui::OpenPopup("Paste from file");
        uiState_->showPasteFileDialog = false;
    }
    if (ImGui::BeginPopupModal("Paste from file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a text file (Applesoft listing, etc.)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", uiState_->pasteDialogPath.c_str());
        if (ImGui::InputText("##PastePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            uiState_->pasteDialogPath = buf;
        else
            uiState_->pasteDialogPath = buf;
        ImGui::Checkbox("Auto-uppercase", &uiState_->pasteAutoUppercase);
        ImGui::Separator();
        if (ImGui::Button("Paste", ImVec2(120, 0))) {
            if (!uiState_->pasteDialogPath.empty()) pasteFromFile(uiState_->pasteDialogPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─── Joystick / paddles ──────────────────────────────────────────────────

void MainWindow::pollJoystickAndPushToMemory()
{
    joystick->poll();
    joystick->autoBindIfUnconfigured();

    // One-shot diagnostic when the bound pad (or its gamepad-mapping status)
    // changes: the kiosk Start-menu only works when gamepad-mapped=yes. If a
    // pad is present but reports "no", GLFW has no standard mapping for it,
    // so use the F1 keyboard fallback (or add an SDL mapping).
    {
        const int  hi = joystick->binding().hostIdx;
        const bool gp = joystick->activeIsGamepad();
        if (hi != uiState_->loggedJoystickHost || gp != uiState_->loggedJoystickGamepad) {
            uiState_->loggedJoystickHost    = hi;
            uiState_->loggedJoystickGamepad = gp;
            if (hi >= 0) {
                pom2::log().info(
                    "Joystick", "bound #" + std::to_string(hi + 1) + " '" +
                    joystick->activeName() + "' gamepad-mapped=" +
                    (gp ? "yes (Start opens kiosk disk menu)"
                        : "no (use F1 for the kiosk disk menu)"));
            } else {
                pom2::log().info("Joystick", "no pad bound");
            }
        }
    }

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. Hold stateMutex
    // while writing: the CPU worker reads paddleValue/paddleButton inside
    // softSwitchAccess under the same lock (during processor.run()), so an
    // unlocked write here is a data race on those non-atomic arrays.
    // While the kiosk disk selector is open the pad drives the menu, not the
    // game: feed the Apple II centered paddles + released buttons so the A/B
    // navigation presses (which share physical buttons with PB0/PB1) don't
    // leak into the running title.
    const bool menuActive = uiState_->kioskMenuOpen;
    const JoystickInput::GamepadPlay play = joystick->play();

    // Menu → game isolation across the close. Circle/Cross double as the
    // menu's B/A and the Apple PB0/PB1, and this poll runs BEFORE
    // updateKioskMenu, so `menuActive` lags the close by a frame: the press
    // that dismissed the menu would land in the game as a fire-button hit.
    // Latch a swallow on the open→closed edge and hold it until every shared
    // button (faces + D-pad) is released. Analog paddles stay live — the
    // stick isn't a menu control and carries no edge.
    // Only gamepad-mapped pads need the latch: raw pads can't drive the menu
    // (nav requires a mapping), so a fire button held across a keyboard-
    // driven close is a legitimate game input, not a menu leftover.
    if (uiState_->kioskMenuWasOpen && !menuActive && play.valid) uiState_->kioskSwallowPad = true;
    uiState_->kioskMenuWasOpen = menuActive;
    if (uiState_->kioskSwallowPad) {
        const bool anyHeld = play.valid
            ? (play.button0 || play.button1 || play.dpadUp || play.dpadDown ||
               play.dpadLeft || play.dpadRight)
            : (joystick->buttonDown(0) || joystick->buttonDown(1) ||
               joystick->buttonDown(2));
        if (!anyHeld) uiState_->kioskSwallowPad = false;
    }
    const bool suppressGame = menuActive || uiState_->kioskSwallowPad;

    {
        auto st = controller->lockState();
        Memory& mem = st.memory();
        for (int i = 0; i < 4; ++i)
            mem.setPaddle(i, menuActive ? 128 : joystick->paddleValue(i));

        if (suppressGame) {
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, false);
        } else if (play.valid) {
            // Gamepad-mapped: only Cross/Circle are Apple game-port buttons;
            // the other face buttons are keyboard keys (below), so PB2 is up.
            mem.setPaddleButton(0, play.button0);   // Circle → PB0
            mem.setPaddleButton(1, play.button1);   // Cross  → PB1
            mem.setPaddleButton(2, false);
        } else {
            // Raw pad (unknown layout): legacy buttons 0/1/2 → PB0/1/2.
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, joystick->buttonDown(i));
        }
    }

    // Keyboard routing for the digital controls — outside stateMutex, since
    // queueKey has its own keyboard lock. Only in-game (menu closed, swallow
    // drained) and only for a gamepad-mapped pad whose layout we can trust.
    //
    // Its own reference, deliberately: the paddle block above reaches Memory
    // through the state lock, this one must NOT hold that lock (queueKey
    // takes `Memory::kbMutex`, the finer-grained one). Sharing a single
    // reference across the two, as this function used to, is what made the
    // split invisible.
    Memory& mem = controller->memory();
    if (suppressGame || !play.valid) {
        // Drop the auto-repeat history so a direction still held from menu
        // navigation re-arms cleanly (press-then-delay) once released.
        for (bool& h : uiState_->padArrowHeld) h = false;
    } else {
        if (play.spaceEdge) mem.queueKey(0x20);   // Square   → SPACE
        if (play.enterEdge) mem.queueKey(0x0D);   // Triangle → RETURN
        // D-pad → Apple II arrow codes (←$08 →$15 ↑$0B ↓$0A) with auto-repeat
        // so a held direction keeps moving, like the //e keyboard.
        const bool    held[4] = { play.dpadUp, play.dpadDown, play.dpadLeft, play.dpadRight };
        const uint8_t code[4] = { 0x0B, 0x0A, 0x08, 0x15 };
        const double  t = ImGui::GetTime();
        for (int i = 0; i < 4; ++i) {
            if (!held[i]) { uiState_->padArrowHeld[i] = false; continue; }
            if (!uiState_->padArrowHeld[i]) {                       // press: fire once
                mem.queueKey(code[i]);
                uiState_->padArrowHeld[i]  = true;
                uiState_->padArrowNextTime[i] = t + 0.35;              // delay before repeat
            } else if (t >= uiState_->padArrowNextTime[i]) {           // held: repeat
                mem.queueKey(code[i]);
                uiState_->padArrowNextTime[i] = t + 0.06;              // ~16/s
            }
        }
    }
}

// Ethernet (Uthernet I / II). Snapshot-under-lock → render → dispatch
// actions under the lock again, the LeChatMauve_ImGui pattern. The card
// pointers are non-owning (SlotBus owns the cards) and are nulled on
// every re-plug, so they are only ever dereferenced inside the lock.
