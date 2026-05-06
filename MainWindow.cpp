// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"
#include "CassetteDevice.h"
#include "Logger.h"
#include "SpeakerDevice.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

MainWindow::MainWindow()
    : memViewer(&controller.memory())
{
    // Memory viewer writes go through Memory::memWrite under stateMutex,
    // so a byte poked from the UI passes through ROM-write protection and
    // any future I/O hooks just like a CPU store would.
    memViewer.setWriteCallback([this](uint16_t a, uint8_t v) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.memory().memWrite(a, v);
    });

    // Probe a few common locations so the binary works whether launched
    // from build/ or the repo root.
    namespace fs = std::filesystem;
    static const char* romCandidates[]      = { "roms/apple2.rom",
                                                "../roms/apple2.rom",
                                                "../../roms/apple2.rom" };
    static const char* charRomCandidates[]  = { "roms/apple2_char.rom",
                                                "../roms/apple2_char.rom",
                                                "../../roms/apple2_char.rom" };

    for (const char* p : romCandidates) {
        if (fs::exists(p)) { romPath = p; break; }
    }
    for (const char* p : charRomCandidates) {
        if (fs::exists(p)) { charRomPath = p; break; }
    }

    if (controller.memory().loadAppleIIRom(romPath.c_str())) {
        romStatus = std::string("loaded: ") + romPath;
    } else {
        romStatus = std::string("NO ROM (") + romPath +
                    ") — only $D000-$FFFF stub is active";
    }
    controller.memory().loadCharRom(charRomPath.c_str());

    // Auto-plug the Disk II in slot 6 if a boot PROM is available. The
    // card is constructed empty (no disk inserted); a `.dsk` can be
    // mounted later via Hardware → Insert Disk.
    {
        static const char* diskRomCandidates[] = {
            "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom"
        };
        for (const char* p : diskRomCandidates) {
            if (fs::exists(p)) { diskRomPath = p; break; }
        }
        auto card = std::make_unique<DiskIICard>();
        if (card->loadBootRom(diskRomPath)) {
            diskRomStatus = std::string("loaded: ") + diskRomPath;
            diskCard = card.get();
            controller.memory().slotBus().plug(6, std::move(card));
        } else {
            diskRomStatus = std::string("NO Disk II PROM (") + diskRomPath + ")";
            // Keep diskCard = nullptr; UI will still render but greyed out.
        }
    }

    // Auto-plug a ProDOS block-device hard disk in slot 5 when the bundled
    // Total Replay HDV is present. Slot 5 is the conventional SmartPort /
    // hard-disk slot and keeps Disk II in slot 6 available for floppies.
    {
        static const char* hdvCandidates[] = {
            "hdv/Total Replay v5.2.hdv",
            "../hdv/Total Replay v5.2.hdv",
            "../../hdv/Total Replay v5.2.hdv"
        };
        for (const char* p : hdvCandidates) {
            if (fs::exists(p)) { hdvPath = p; break; }
        }
        auto card = std::make_unique<ProDOSHardDiskCard>();
        if (card->loadImage(hdvPath)) {
            hdvStatus = std::string("loaded: ") + hdvPath;
            hdvCard = card.get();
            controller.memory().slotBus().plug(5, std::move(card));
        } else {
            hdvStatus = std::string("NO HDV (") + hdvPath + ")";
        }
    }

    // Plug the Le Chat Mauve RGB video card in slot 7. It's the convention
    // for Apple II video adapters (Apple Color Card, Video-7) and matches
    // the historical placement in French II+ machines that shipped with a
    // Féline pre-installed. The card has no boot ROM and exposes nothing on
    // the bus — Apple2Display queries it directly to choose the rendering
    // path. Default mode at construction is COL140 (RGB 16 colors), so
    // every HGR-capable program is rendered with the clean palette out of
    // the box; AN3+80COL software can switch modes at runtime via the FIFO.
    {
        auto card = std::make_unique<LeChatMauveCard>();
        chatMauveCard = card.get();
        controller.memory().slotBus().plug(7, std::move(card));
        display.setChatMauveCard(chatMauveCard);
    }

    if (hdvCard && hdvCard->isImageLoaded()) {
        bootHdvImage();
    } else {
        controller.cpu().hardReset();
    }
    controller.setMode(EmulationController::Mode::Running);
    controller.start();
}

MainWindow::~MainWindow()
{
    controller.stop();
}

// ─── Keyboard ─────────────────────────────────────────────────────────────

void MainWindow::injectAscii(uint8_t apple2Code)
{
    controller.memory().queueKey(apple2Code);
}

void MainWindow::onChar(unsigned int codepoint)
{
    // Apple II accepts the full ASCII range (uppercase and lowercase). We
    // forward the codepoint as-is — Applesoft and the Monitor pick whichever
    // case the user typed.
    if (codepoint >= 0x20 && codepoint < 0x80) {
        injectAscii(static_cast<uint8_t>(codepoint));
    }
}

void MainWindow::onKey(int key, int /*scancode*/, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Ctrl-V intercepts the host shortcut: paste system clipboard into
    // the Apple II keyboard buffer rather than injecting raw $16. The
    // Apple II's own Ctrl-V (rarely used) can still be reached via the
    // Edit menu or via Ctrl-Shift-V if a future version chooses to map it.
    if (ctrl && key == GLFW_KEY_V) {
        pasteFromClipboard();
        return;
    }

    switch (key) {
        case GLFW_KEY_ENTER:        injectAscii(0x0D); break;
        case GLFW_KEY_BACKSPACE:    injectAscii(0x08); break;
        case GLFW_KEY_LEFT:         injectAscii(0x08); break;
        case GLFW_KEY_RIGHT:        injectAscii(0x15); break;
        case GLFW_KEY_UP:           injectAscii(0x0B); break;
        case GLFW_KEY_DOWN:         injectAscii(0x0A); break;
        case GLFW_KEY_ESCAPE:       injectAscii(0x1B); break;
        case GLFW_KEY_TAB:          injectAscii(0x09); break;
        case GLFW_KEY_F11:          controller.softReset(); break;
        case GLFW_KEY_F12:          controller.hardReset(); break;
        default:
            // Ctrl-A..Ctrl-Z generate $01..$1A — these matter for Applesoft
            // (Ctrl-C breaks out of a running program, Ctrl-G beeps, etc.)
            if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
                injectAscii(static_cast<uint8_t>(key - GLFW_KEY_A + 1));
            }
            break;
    }
}

// ─── Paste ───────────────────────────────────────────────────────────────

void MainWindow::pasteFromClipboard()
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        tapeStatusMessage = "Paste: clipboard is empty";
        tapeStatusUntil   = lastFrameTime + 3.0;
        return;
    }
    std::string text = clip;
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller.memory().pasteText(text);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars queued from clipboard", queued);
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::pasteFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        tapeStatusMessage = "Paste: cannot open " + path;
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller.memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars from %s", queued, path.c_str());
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

// ─── Texture upload ──────────────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

void MainWindow::uploadScreenTexture()
{
    if (screenTexture == 0) {
        glGenTextures(1, &screenTexture);
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     Apple2Display::kWidth, Apple2Display::kHeight,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    {
        // Render under stateMutex so we get a consistent snapshot of RAM
        // (otherwise the CPU may be mid-frame with the text screen half
        // updated, producing tearing).
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        display.render(controller.memory());
    }

    glBindTexture(GL_TEXTURE_2D, screenTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    Apple2Display::kWidth, Apple2Display::kHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, display.pixels());
}

// ─── Render passes ───────────────────────────────────────────────────────

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Reload ROM")) {
            bool ok = false;
            std::string err;
            {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                ok = controller.memory().loadAppleIIRom(romPath.c_str());
                if (ok) {
                    controller.hardReset();
                } else {
                    err = controller.memory().getLastError();
                }
            }
            if (ok) romStatus = std::string("loaded: ") + romPath;
            else    romStatus = err;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (window) glfwSetWindowShouldClose(window, 1);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Paste from clipboard", "Ctrl+V"))
            pasteFromClipboard();
        if (ImGui::MenuItem("Paste from file..."))
            showPasteFileDialog = true;
        ImGui::Separator();
        const size_t pending = controller.memory().pendingPasteSize();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::MenuItem("Cancel pending paste")) {
            controller.memory().cancelPaste();
            tapeStatusMessage = "Paste cancelled";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Auto-uppercase pasted text", nullptr, &pasteAutoUppercase);
        if (pending > 0) {
            ImGui::Separator();
            ImGui::TextDisabled("(%zu chars pending)", pending);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        const auto m = controller.getMode();
        if (ImGui::MenuItem("Run", nullptr, m == EmulationController::Mode::Running)) {
            controller.setMode(EmulationController::Mode::Running);
        }
        if (ImGui::MenuItem("Pause", nullptr, m == EmulationController::Mode::Stopped)) {
            controller.setMode(EmulationController::Mode::Stopped);
        }
        if (ImGui::MenuItem("Step (one instr)")) controller.requestStep();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset (Ctrl-Reset)",     "F11")) controller.softReset();
        if (ImGui::MenuItem("Hard reset",             "F12")) controller.hardReset();
        if (ImGui::MenuItem("Cold boot (wipe RAM)"))          controller.coldBoot();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Presets")) {
        if (ImGui::MenuItem("Apple II Original (1977)")) {
            controller.setCyclesPerFrame(17045);   // 1.023 MHz native
            pixelScale   = 2.0f;
            controller.hardReset();                // also resets soft switches → TEXT, page 1
        }
        if (ImGui::MenuItem("Apple II Plus (1979)")) {
            controller.setCyclesPerFrame(17045);
            pixelScale   = 2.0f;
            controller.hardReset();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Display")) {
        ImGui::SliderFloat("Pixel scale", &pixelScale, 1.0f, 4.0f, "%.1fx");

        ImGui::Separator();
        ImGui::TextDisabled("Hi-res mode");
        const Apple2Display::HiResMode cur = display.getHiResMode();
        if (ImGui::MenuItem("Color NTSC", nullptr,
                            cur == Apple2Display::HiResMode::ColorNTSC))
            display.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        // Le Chat Mauve RGB — clean Péritel decode, two distinct grays,
        // no inter-byte fringing. Greyed out if the slot-7 card isn't
        // plugged (the Apple II would just see composite video).
        ImGui::BeginDisabled(chatMauveCard == nullptr);
        if (ImGui::MenuItem("Le Chat Mauve (RGB)", nullptr,
                            cur == Apple2Display::HiResMode::ChatMauveRGB))
            display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Mono White",  nullptr,
                            cur == Apple2Display::HiResMode::MonoWhite))
            display.setHiResMode(Apple2Display::HiResMode::MonoWhite);
        if (ImGui::MenuItem("Mono Green (P31)", nullptr,
                            cur == Apple2Display::HiResMode::MonoGreen))
            display.setHiResMode(Apple2Display::HiResMode::MonoGreen);
        if (ImGui::MenuItem("Mono Amber",  nullptr,
                            cur == Apple2Display::HiResMode::MonoAmber))
            display.setHiResMode(Apple2Display::HiResMode::MonoAmber);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Hardware")) {
        ImGui::MenuItem("Cassette deck", nullptr, &showCassetteDeck);
        ImGui::MenuItem("Disk II (slot 6)", nullptr, &showDiskPanel);
        ImGui::MenuItem("Le Chat Mauve (slot 7)", nullptr, &showChatMauvePanel);
        ImGui::MenuItem("Joystick", nullptr, &showJoystickPanel);
        ImGui::Separator();
        ImGui::BeginDisabled(!hdvCard || !hdvCard->isImageLoaded());
        if (ImGui::MenuItem("Boot Total Replay HDV (slot 5)")) {
            bootHdvImage();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(diskCard == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk)...")) {
            showDiskInsertDialog = true;
            if (diskDialogPath.empty()) diskDialogPath = "disks/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            diskCard && diskCard->isDiskLoaded())) {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            diskCard->ejectDisk();
            tapeStatusMessage = "Disk ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("Memory viewer", nullptr, &showMemViewer);
        ImGui::Separator();
        ImGui::MenuItem("Memory Map Bar",              nullptr, &showMemoryBar);
        ImGui::MenuItem("Memory Map Bar (Horizontal)", nullptr, &showMemoryBarH);
        ImGui::MenuItem("Memory Map Grid",             nullptr, &showMemoryGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About POM2")) showAbout = true;
        ImGui::EndMenu();
    }

    // Status pill on the right.
    {
        const char* modeStr = "?";
        switch (controller.getMode()) {
            case EmulationController::Mode::Running: modeStr = "RUN";  break;
            case EmulationController::Mode::Stopped: modeStr = "STOP"; break;
            case EmulationController::Mode::Step:    modeStr = "STEP"; break;
        }
        const auto state = controller.memory().getDisplayState();
        const char* gfx = state.textMode ? "TEXT"
                        : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                         : (state.mixedMode ? "LGR+TXT" : "LGR");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %s | %s | %s", modeStr, gfx, romStatus.c_str());
        ImGui::TextDisabled("%s", buf);
    }
    ImGui::EndMainMenuBar();
}

void MainWindow::renderScreenWindow()
{
    // Curated startup layout: Apple II Screen anchors the left ~75 % of
    // the GLFW canvas, leaving the right margin for the Disk II + Emulation
    // stack. FirstUseEver lets the user drag/resize freely after the first
    // frame.
    ImGui::SetNextWindowPos (ImVec2(5,    30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1080, 960), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    if (ImGui::Begin("Apple II Screen")) {
        uploadScreenTexture();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float scale = std::min(avail.x / Apple2Display::kWidth,
                               avail.y / Apple2Display::kHeight);
        scale = std::max(scale, 1.0f);
        ImVec2 size(Apple2Display::kWidth  * scale,
                    Apple2Display::kHeight * scale);

        ImVec2 cur = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(
            cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
            cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

        ImGui::Image(static_cast<ImTextureID>(screenTexture), size);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainWindow::renderControlsWindow()
{
    ImGui::SetNextWindowPos (ImVec2(1095, 780), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  210), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Emulation")) {
        // CPU speed control.
        int cycPerFrame = controller.getCyclesPerFrame();
        const int oneX  = 17045;
        const int twoX  = 34091;
        const int maxX  = 1'000'000;
        ImGui::Text("Speed: %d cycles/frame (~%.2f MHz)",
                    cycPerFrame, cycPerFrame * 60.0 / 1e6);
        if (ImGui::Button("1x"))  {
            controller.setCyclesPerFrame(oneX);
            if (diskTurboActive) diskSavedCyclesPerFrame = oneX;
        }
        ImGui::SameLine();
        if (ImGui::Button("2x"))  {
            controller.setCyclesPerFrame(twoX);
            if (diskTurboActive) diskSavedCyclesPerFrame = twoX;
        }
        ImGui::SameLine();
        if (ImGui::Button("MAX")) {
            controller.setCyclesPerFrame(maxX);
            if (diskTurboActive) diskSavedCyclesPerFrame = maxX;
        }

        ImGui::Separator();
        ImGui::Text("PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X",
            controller.cpu().getProgramCounter(),
            controller.cpu().getAccumulator(),
            controller.cpu().getXRegister(),
            controller.cpu().getYRegister(),
            controller.cpu().getStackPointer());
        ImGui::Text("Cycles: %llu",
            (unsigned long long)controller.memory().getCycleCounter());
        ImGui::Text("Speaker toggles: %llu",
            (unsigned long long)controller.memory().getSpeakerToggleCount());

        // Speaker mix controls — directly bound to SpeakerDevice atomics.
        SpeakerDevice& spk = controller.speaker();
        float spkVol = spk.getVolume();
        if (ImGui::SliderFloat("Speaker vol", &spkVol, 0.0f, 2.0f, "%.2f"))
            spk.setVolume(spkVol);
        bool spkMute = spk.isMuted();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mute##spk", &spkMute)) spk.setMuted(spkMute);

        ImGui::Separator();
        const size_t pendingPaste = controller.memory().pendingPasteSize();
        if (pendingPaste > 0) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.15f, 1.0f),
                               "Paste in flight: %zu chars", pendingPaste);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##paste")) {
                controller.memory().cancelPaste();
            }
        }
        ImGui::TextWrapped("ROM: %s", romStatus.c_str());
        if (!hdvStatus.empty()) ImGui::TextWrapped("HDV: %s", hdvStatus.c_str());
    }
    ImGui::End();
}

void MainWindow::bootHdvImage()
{
    if (!hdvCard || !hdvCard->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.memory().clearRam();
        controller.memory().resetSoftSwitches();
        controller.memory().slotBus().reset();
        controller.cpu().hardReset();
        controller.cpu().setProgramCounter(0xC500);
        controller.setMode(EmulationController::Mode::Running);
    }
    tapeStatusMessage = "Booting HDV: " + hdvCard->getImagePath();
    tapeStatusUntil   = lastFrameTime + 4.0;
    pom2::log().info("HDV", "Boot via slot 5 ROM: " + hdvCard->getImagePath());
}

void MainWindow::renderPasteFileDialog()
{
    if (showPasteFileDialog) {
        ImGui::OpenPopup("Paste from file");
        showPasteFileDialog = false;
    }
    if (ImGui::BeginPopupModal("Paste from file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a text file (Applesoft listing, etc.)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", pasteDialogPath.c_str());
        if (ImGui::InputText("##PastePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            pasteDialogPath = buf;
        else
            pasteDialogPath = buf;
        ImGui::Checkbox("Auto-uppercase", &pasteAutoUppercase);
        ImGui::Separator();
        if (ImGui::Button("Paste", ImVec2(120, 0))) {
            if (!pasteDialogPath.empty()) pasteFromFile(pasteDialogPath);
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
    joystick.poll();
    joystick.autoBindIfUnconfigured();

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. No need to
    // hold stateMutex — these setters write atomic-friendly scalars.
    Memory& mem = controller.memory();
    for (int i = 0; i < 4; ++i)  mem.setPaddle(i, joystick.paddleValue(i));
    for (int i = 0; i < 3; ++i)  mem.setPaddleButton(i, joystick.buttonDown(i));
}

void MainWindow::renderJoystickPanelWindow()
{
    if (!showJoystickPanel) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick.deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick.binding();
    snap.hostIdx  = cf.hostIdx;
    snap.deadzone = cf.deadzone;
    snap.invert   = cf.invert;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick.paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick.buttonDown(i);

    auto result = joystickPanel.render("Joystick", showJoystickPanel, snap);
    if (result.changed) {
        auto& bind = joystick.binding();
        bind.hostIdx  = result.hostIdx;
        bind.deadzone = result.deadzone;
        bind.invert   = result.invert;
    }
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!showChatMauvePanel) return;

    pom2::LeChatMauve_ImGui::Snapshot snap;
    if (chatMauveCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        snap.plugged   = true;
        snap.mode      = chatMauveCard->currentMode();
        snap.fifoBits  = chatMauveCard->fifoBits();
        snap.eightyCol = chatMauveCard->eightyCol();
        snap.an3High   = chatMauveCard->an3High();
    }

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel.render("Le Chat Mauve (slot 7)",
                                        showChatMauvePanel, snap);

    if (chatMauveCard && result.requestOverride) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        chatMauveCard->overrideMode(result.overrideTo);
    }
    if (chatMauveCard && result.requestReset) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        chatMauveCard->onReset();
    }
}

// ─── Disk II ─────────────────────────────────────────────────────────────

void MainWindow::renderDiskPanelWindow()
{
    if (!showDiskPanel) return;

    pom2::DiskController_ImGui::DriveSnapshot snap;
    if (diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        snap.bootRomLoaded = diskCard->hasBootRom();
        snap.diskLoaded    = diskCard->isDiskLoaded();
        snap.motorOn       = diskCard->isMotorOn();
        snap.track         = diskCard->getCurrentTrack();
        snap.halfTrack     = diskCard->getHalfTrack();
        snap.trackPos      = diskCard->getTrackPosition();
        snap.diskPath      = diskCard->getDiskPath();
    }
    snap.turboWhileMotor = diskTurboWhileMotor;
    snap.turboActive     = diskTurboActive;

    // Disk library — scan disks/ for .dsk and .do files. Cheap (a few
    // dirent reads per frame), but sorted alphabetically so the list
    // doesn't reshuffle each time the OS hands us a different order.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks", "../disks", "../../disks" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = entry.path().filename().string();
                e.fullPath    = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // Auto-turbo: while the motor is spinning, replace the user's chosen
    // cyclesPerFrame with a "MAX" value (~60 MHz emulated) so the LSS's
    // nibble pacing is multiplied by the same factor — the disk reads
    // proceed at ~1 second of wall time for a full DOS 3.3 boot. When the
    // motor stops (DOS finished loading) we restore the prior setting so
    // typed-in BASIC programs run at the natural Apple II clock.
    if (diskCard && diskTurboWhileMotor) {
        const bool wantTurbo = snap.motorOn;
        if (wantTurbo && !diskTurboActive) {
            diskSavedCyclesPerFrame = controller.getCyclesPerFrame();
            controller.setCyclesPerFrame(1'000'000);
            diskTurboActive = true;
        } else if (!wantTurbo && diskTurboActive) {
            controller.setCyclesPerFrame(diskSavedCyclesPerFrame);
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        // Toggle was switched off mid-spin — drop back to saved speed.
        controller.setCyclesPerFrame(diskSavedCyclesPerFrame);
        diskTurboActive = false;
    }

    // Curated startup pos/size — height accommodates the 2×-tall library
    // list (360 px child) without forcing the panel itself to scroll.
    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  725), ImGuiCond_FirstUseEver);
    auto result = diskPanel.render("Disk II (slot 6)", showDiskPanel, snap);
    if (result.turboToggleChanged) {
        diskTurboWhileMotor = result.turboNewValue;
    }
    if (result.requestInsertDialog) {
        showDiskInsertDialog = true;
        if (diskDialogPath.empty()) diskDialogPath = "disks/";
    }
    if (result.requestEject && diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard->ejectDisk();
        tapeStatusMessage = "Disk ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (result.requestBoot && diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard->seekTrack0();
        controller.cpu().setProgramCounter(0xC600);
        controller.setMode(EmulationController::Mode::Running);
        tapeStatusMessage = "Boot: PC → $C600";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (!result.requestInsertAndBoot.empty() && diskCard) {
        // Library-click "insert + boot" — mirrors the manual flow of
        // "wipe RAM → reset slots/CPU → insert new disk → jump straight
        // into the PROM at $C600". Doing it as one mutex-held atomic
        // operation prevents the worker thread from running a single
        // instruction with stale state (e.g., the old disk still
        // mounted but the CPU already at $C600).
        const std::string path = result.requestInsertAndBoot;
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            controller.memory().clearRam();
            controller.memory().resetSoftSwitches();
            controller.memory().slotBus().reset();
            ok = diskCard->insertDisk(path);
            if (ok) {
                diskCard->seekTrack0();
                controller.cpu().hardReset();
                controller.cpu().setProgramCounter(0xC600);
                controller.setMode(EmulationController::Mode::Running);
                pom2::log().info("Disk II",
                    std::string("Library click → insert + boot: ") + path);
            } else {
                err = diskCard->getLastError();
            }
        }
        if (ok) {
            tapeStatusMessage = "Booting: " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    if (showDiskInsertDialog) {
        ImGui::OpenPopup("Insert disk image");
        showDiskInsertDialog = false;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("Path to a 143 360-byte .dsk / .do image"
                           " (DOS 3.3 sector order, read-only)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", diskDialogPath.c_str());
    if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        diskDialogPath = buf;
    else
        diskDialogPath = buf;

    // Quick list of .dsk files in disks/ (mirrors the cassette dialog).
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks", "../disks", "../../disks" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".dsk" && ext != ".do") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                diskDialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (diskCard && !diskDialogPath.empty()) {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            if (diskCard->insertDisk(diskDialogPath)) {
                tapeStatusMessage = "Disk inserted: " + diskDialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + diskCard->getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ImGui::OpenPopup("About POM2");
    if (ImGui::BeginPopupModal("About POM2", &showAbout,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("POM2 v0.1");
        ImGui::Text("Apple II / II+ emulator (Dear ImGui, MOS 6502)");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Apple II / II+ emulator. MOS 6502, 48 KB RAM,"
            " text / lo-res / hi-res display, soft-switch I/O.");
        ImGui::Spacing();
        ImGui::Text("F11 = Reset (Ctrl-Reset)   F12 = Hard reset");
        ImGui::Text("ESC, arrows, Ctrl-A..Z map straight to the keyboard");
        ImGui::Spacing();
        if (ImGui::Button("Close")) showAbout = false;
        ImGui::EndPopup();
    }
}

void MainWindow::renderMemoryViewerWindow()
{
    if (!showMemViewer) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &showMemViewer)) {
        // Hold the state mutex briefly so the snapshot the viewer reads
        // (Memory::data()) is consistent — no torn writes mid-row.
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        memViewer.render();
    }
    ImGui::End();
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!showCassetteDeck) return;

    // Build the deck snapshot under stateMutex — cheap enough that holding
    // the emul lock for the time it takes to copy a dozen scalars is fine.
    pom2::CassetteDeck_ImGui::DeckSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        const CassetteDevice& d = controller.cassette();
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
    auto result = cassetteDeck.render("Cassette Deck",
                                      showCassetteDeck,
                                      &controller,
                                      snap,
                                      deltaSeconds);

    if (result.requestLoadDialog) {
        showTapeLoadDialog = true;
        if (tapeDialogPath.empty()) tapeDialogPath = "cassettes/";
    }
    if (result.requestSaveDialog) {
        showTapeSaveDialog = true;
        if (tapeDialogPath.empty()) tapeDialogPath = "cassettes/recording.aci";
    }
    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 4.0;  // show for 4 seconds
    }
}

void MainWindow::renderTapeFileDialogs()
{
    auto pathInput = [](const char* label) {
        // Minimal text-only path widget — POM2 doesn't pull in nativefiledialog.
        // The user types a path; convenience dirs/files can be appended later.
        ImGui::TextUnformatted(label);
    };

    if (showTapeLoadDialog) {
        ImGui::OpenPopup("Load Tape");
        showTapeLoadDialog = false;
    }
    if (ImGui::BeginPopupModal("Load Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Tape file path (.aci / .wav / .mp3 / .ogg / .flac)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", tapeDialogPath.c_str());
        if (ImGui::InputText("##LoadPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            tapeDialogPath = buf;
        else
            tapeDialogPath = buf;

        // Quick list of cassettes/ directory contents (one click → fill).
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "cassettes", "../cassettes", "../../cassettes" }) {
            if (!fs::is_directory(dir, ec)) continue;
            ImGui::Separator();
            ImGui::TextDisabled("%s/", dir);
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".aci" && ext != ".wav" && ext != ".mp3" &&
                    ext != ".ogg" && ext != ".flac") continue;
                const std::string name = entry.path().filename().string();
                if (ImGui::Selectable(name.c_str()))
                    tapeDialogPath = entry.path().string();
            }
            break;  // first existing candidate dir wins
        }

        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (controller.loadTape(tapeDialogPath)) {
                tapeStatusMessage = "Tape loaded: " + tapeDialogPath;
            } else {
                tapeStatusMessage = "Load failed: " + controller.cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showTapeSaveDialog) {
        ImGui::OpenPopup("Save Tape");
        showTapeSaveDialog = false;
    }
    if (ImGui::BeginPopupModal("Save Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Output file path (.aci or .wav)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", tapeDialogPath.c_str());
        if (ImGui::InputText("##SavePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            tapeDialogPath = buf;
        else
            tapeDialogPath = buf;

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (controller.saveTape(tapeDialogPath)) {
                tapeStatusMessage = "Tape saved: " + tapeDialogPath;
            } else {
                tapeStatusMessage = "Save failed: " + controller.cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Status line near the bottom.
    if (!tapeStatusMessage.empty() && lastFrameTime < tapeStatusUntil) {
        ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 36),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##TapeStatus", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(tapeStatusMessage.c_str());
        }
        ImGui::End();
    }
}

void MainWindow::render()
{
    // Track wallclock between frames so the deck counter / armed pulse /
    // status overlay can age correctly.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const float deltaSeconds = static_cast<float>(std::max(0.0, now - lastFrameTime));
    lastFrameTime = now;

    pollJoystickAndPushToMemory();

    renderMenuBar();
    renderScreenWindow();
    renderControlsWindow();
    renderMemoryViewerWindow();
    if (showMemoryBar)  renderMemoryBarWindow();
    if (showMemoryBarH) renderMemoryBarHorizontalWindow();
    if (showMemoryGrid) renderMemoryGridWindow();
    renderCassetteDeckWindow(deltaSeconds);
    renderTapeFileDialogs();
    renderPasteFileDialog();
    renderDiskPanelWindow();
    renderDiskFileDialog();
    renderChatMauvePanelWindow();
    renderJoystickPanelWindow();
    renderAboutDialog();
}
