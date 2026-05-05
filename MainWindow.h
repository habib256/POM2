// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_MAIN_WINDOW_H
#define POM2_MAIN_WINDOW_H

#include "Apple2Display.h"
#include "CassetteDeck_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "MemoryViewer_ImGui.h"

#include <cstdint>
#include <string>

struct GLFWwindow;

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    EmulationController& emul() { return controller; }

    void setGlfwWindow(GLFWwindow* w) { window = w; }
    void render();

    // Hooks installed by main.cpp into GLFW callbacks.
    void onChar(unsigned int codepoint);
    void onKey (int key, int scancode, int action, int mods);

private:
    EmulationController          controller;
    Apple2Display                display;
    MemoryViewer_ImGui           memViewer;
    pom2::CassetteDeck_ImGui     cassetteDeck;
    pom2::DiskController_ImGui   diskPanel;
    pom2::JoystickPanel_ImGui    joystickPanel;
    DiskIICard*                  diskCard = nullptr;   // non-owning, owned by SlotBus
    JoystickInput                joystick;
    GLFWwindow*                  window = nullptr;

    unsigned int screenTexture = 0;     // GL texture name (lazy)
    float        pixelScale    = 2.0f;
    bool         showMemViewer = false;
    bool         showCassetteDeck = true;
    bool         showDiskPanel = true;
    bool         showJoystickPanel = false;

    // Disk II insert dialog state.
    bool        showDiskInsertDialog = false;
    std::string diskDialogPath;
    std::string diskRomPath  = "roms/disk2.rom";
    std::string diskRomStatus;
    // Auto-turbo while the Disk II motor is spinning. Real Apple II boot
    // takes 10-15 s at 1 MHz; bumping to ~60 MHz emulated for the duration
    // of the read drops it to <1 s. Off restores the user's chosen speed.
    bool        diskTurboWhileMotor = true;
    int         diskSavedCyclesPerFrame = 17045;
    bool        diskTurboActive = false;

    // Cassette load/save dialog state.
    bool        showTapeLoadDialog = false;
    bool        showTapeSaveDialog = false;
    std::string tapeDialogPath;
    std::string tapeStatusMessage;
    double      tapeStatusUntil = 0.0;
    double      lastFrameTime   = 0.0;

    // Paste-from-file dialog state.
    bool        showPasteFileDialog = false;
    std::string pasteDialogPath;
    bool        pasteAutoUppercase  = false;

    // Boot-time ROM probing.
    std::string romPath = "roms/apple2.rom";
    std::string charRomPath = "roms/apple2_char.rom";
    std::string romStatus = "no ROM loaded";

    bool showAbout = false;

    void renderMenuBar();
    void renderScreenWindow();
    void renderControlsWindow();
    void renderMemoryViewerWindow();
    void renderCassetteDeckWindow(float deltaSeconds);
    void renderTapeFileDialogs();
    void renderPasteFileDialog();
    void renderDiskPanelWindow();
    void renderDiskFileDialog();
    void renderJoystickPanelWindow();
    void pollJoystickAndPushToMemory();
    void renderAboutDialog();

    // Paste helpers — feed text into the keyboard buffer.
    void pasteFromClipboard();
    void pasteFromFile(const std::string& path);

    void uploadScreenTexture();

    // Translate a GLFW key/codepoint into an ASCII byte and feed it to
    // Memory::queueKey(). Memory's strobe handling is hardware-faithful;
    // we just hand it the character.
    void injectAscii(uint8_t ascii);
};

#endif // POM2_MAIN_WINDOW_H
