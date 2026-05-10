// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_MAIN_WINDOW_H
#define POM2_MAIN_WINDOW_H

#include "Apple2Display.h"
#include "CassetteDeck_ImGui.h"
#include "ClockCard.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "MemoryViewer_ImGui.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SuperSerialCard.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

class MainWindow
{
public:
    /// `forceIIPlus`: when true, skip the auto-probe for `roms/apple2e.rom`
    /// even if it is present and load `roms/apple2.rom` instead. Used by
    /// the `--ii-plus` CLI flag to fall back to II+ mode for software
    /// (e.g. Copy II Plus 8.3) that boots cleanly under the legacy ROM.
    explicit MainWindow(bool forceIIPlus = false);
    ~MainWindow();

    EmulationController& emul() { return controller; }

    /// Live access to the framebuffer renderer. Used by main() to honour
    /// the `--display <mode>` CLI flag during Phase B.
    Apple2Display& displayRef() { return display; }

    void setGlfwWindow(GLFWwindow* w) { window = w; }
    void render();

    // Hooks installed by main.cpp into GLFW callbacks.
    void onChar(unsigned int codepoint);
    void onKey (int key, int scancode, int action, int mods);
    /// GLFW cursor-position callback. Window-relative coordinates.
    /// Routes to the Apple II Mouse Card when the cursor is inside
    /// the Apple II Screen widget; otherwise no-op (ImGui handles it).
    void onMouseMove  (double x, double y);
    void onMouseButton(int button, int action);

    // Apple II memory map region — used by the bar / grid widgets in
    // MainWindow_MemoryMaps.cpp. Public so file-local helpers in that TU
    // can take a `const std::vector<MemRegion>&`.
    struct MemRegion { uint16_t start, end; ImU32 color; const char* label; };

private:
    EmulationController          controller;
    Apple2Display                display;
    MemoryViewer_ImGui           memViewer;
    pom2::Settings               settings;
    pom2::CassetteDeck_ImGui     cassetteDeck;
    pom2::DiskController_ImGui   diskPanel;
    pom2::HdvController_ImGui    hdvPanel;
    pom2::JoystickPanel_ImGui    joystickPanel;
    pom2::LeChatMauve_ImGui      chatMauvePanel;
    DiskIICard*                  diskCard = nullptr;       // non-owning, owned by SlotBus
    ProDOSHardDiskCard*          hdvCard = nullptr;        // non-owning, owned by SlotBus
    LeChatMauveCard*             chatMauveCard = nullptr;  // non-owning, owned by SlotBus
    SuperSerialCard*             sscCard = nullptr;        // non-owning, owned by SlotBus
    ClockCard*                   clockCard = nullptr;      // non-owning, owned by SlotBus
    MouseCard*                   mouseCard = nullptr;      // non-owning, owned by SlotBus
    MockingboardCard*            mockingboardCard = nullptr; // non-owning, owned by SlotBus
    /// Status of the Mouse Card ROM probe — used by the Slot
    /// Configuration UI to indicate whether 'mouse' is selectable.
    /// "" = not yet probed, "loaded: <paths>" = ready, otherwise the
    /// failure message.
    std::string                  mouseRomStatus;

    /// Canonical per-slot card-type strings, as resolved by
    /// `plugSlotsFromSettings()` (and persisted on shutdown). Index 0 is
    /// the language-card slot and is always empty here.
    /// Values: "", "diskii", "hdv", "ssc", "clock", "chatmauve", "mouse".
    std::array<std::string, 8> slotCards{};

    JoystickInput                joystick;
    GLFWwindow*                  window = nullptr;

    unsigned int screenTexture = 0;     // GL texture name (lazy)
    int          screenTextureWidth  = 0;
    int          screenTextureHeight = 0;
    float        pixelScale    = 2.0f;
    bool         showMemViewer = false;
    bool         showMemoryBar      = false;   // tall vertical map
    bool         showMemoryBarH     = false;   // wide-short horizontal variant
    bool         showMemoryGrid     = false;   // 16×16 page grid
    // Default UI layout: Apple II Screen on the left, HDV top-right,
    // Disk II bottom-right. Every other panel (Emulation controls,
    // Cassette deck, Memory viewers, Joystick, Le Chat Mauve) starts
    // hidden — toggle from the Debug / Hardware menus.
    bool         showCassetteDeck = false;
    bool         showDiskPanel = true;
    bool         showHdvPanel  = true;
    bool         showJoystickPanel = false;
    bool         showChatMauvePanel = false;
    bool         showSscPanel       = false;
    bool         showEmulationPanel = false;
    bool         showSlotConfigPanel = false;
    int          sscPortInput       = SuperSerialCard::kDefaultPort;

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

    // ProDOS hard disk / HDV state. The default mounted image is whatever
    // the constructor finds first under hdv/ (alphabetical) — no auto-boot.
    std::string hdvPath;
    std::string hdvStatus;
    bool        showHdvMountDialog = false;
    std::string hdvDialogPath;

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

    // ── Mouse Card host-input plumbing (Phase 5) ─────────────────────
    // Apple II Screen widget rect, window-relative. Updated every
    // frame by `renderScreenWindow()` so the GLFW cursor-pos callback
    // (which fires async to the render loop) can decide whether to
    // route motion to the Mouse Card.
    ImVec2 screenRectMin{ 0, 0 };
    ImVec2 screenRectMax{ 0, 0 };
    // Running 8-bit Apple II "mouse units" counter — mirrors MAME's
    // IPT_MOUSE_X/Y (0..0xFF wrapping). The MCU firmware computes
    // deltas with 8-bit subtraction.
    uint8_t mouseAppleX = 0;
    uint8_t mouseAppleY = 0;
    // Last GLFW cursor position; used to compute per-frame deltas.
    double  lastMouseHostX = 0;
    double  lastMouseHostY = 0;
    bool    mouseInited    = false;
    bool    mouseButtonHeld = false;

    /// Populate the SlotBus from `slot_1_card`..`slot_7_card` settings,
    /// instantiating each card with its slot number. Falls back to legacy
    /// defaults (DiskII=6, HDV=5, SSC=2, Clock=4, ChatMauve=7) when a
    /// slot key is absent. Validates uniqueness — duplicate card-type
    /// requests log a warning and skip the second instance. Populates the
    /// raw `*Card` pointer fields and the `slotCards[]` index.
    void plugSlotsFromSettings();

    void renderMenuBar();
    void renderScreenWindow();
    void renderControlsWindow();
    void renderMemoryViewerWindow();
    // Memory map visualisations — implemented in MainWindow_MemoryMaps.cpp.
    std::vector<MemRegion> buildMemoryRegions();
    void renderMemoryBarWindow();
    void renderMemoryBarHorizontalWindow();
    void renderMemoryGridWindow();
    void renderCassetteDeckWindow(float deltaSeconds);
    void renderTapeFileDialogs();
    void renderPasteFileDialog();
    void renderDiskPanelWindow();
    void renderDiskFileDialog();
    void renderHdvPanelWindow();
    void renderHdvFileDialog();
    void renderChatMauvePanelWindow();
    void renderSscPanelWindow();
    void renderJoystickPanelWindow();
    void pollJoystickAndPushToMemory();
    void renderAboutDialog();
    /// Slot Configuration panel. Implemented in MainWindow_Slots.cpp.
    void renderSlotConfigPanel();
    /// Tear down the SlotBus and re-run plugSlotsFromSettings(). Called
    /// by the Slot Configuration panel's Apply button. Stops the
    /// emulation worker around the rebuild so card destructors / new
    /// constructors don't race against a running CPU thread.
    void restartEmulationFromSettings();

    // Paste helpers — feed text into the keyboard buffer.
    void pasteFromClipboard();
    void pasteFromFile(const std::string& path);

    // HDV helper — cold-boot through the slot-5 ProDOS block-device ROM.
    void bootHdvImage();

    // Write the current display framebuffer to ./screenshot_NNN.ppm. The
    // sequence number auto-increments to avoid overwriting prior captures
    // within the same session.
    void saveScreenshot();

    void uploadScreenTexture();

    // Translate a GLFW key/codepoint into an ASCII byte and feed it to
    // Memory::queueKey(). Memory's strobe handling is hardware-faithful;
    // we just hand it the character.
    void injectAscii(uint8_t ascii);
};

#endif // POM2_MAIN_WINDOW_H
