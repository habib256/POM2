// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// UI-only state shared by the MainWindow composition TUs.  Keeping it behind
// an opaque pointer in MainWindow.h prevents every public consumer from
// inheriting the full panel registry and gives future panel hosts a state
// object they can own independently of MainWindow.

#ifndef POM2_MAIN_WINDOW_UI_STATE_H
#define POM2_MAIN_WINDOW_UI_STATE_H

#include "PrinterScreenDump.h"
#include "imgui.h"

#include <cstdint>
#include <array>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pom2 {

struct MainWindowUiState {
    bool show3dVoxel = false;
    bool showVoxelSettings = false;
    bool showNtscSettings = false;

    bool showMemViewer = false;
    bool showMemoryBar = false;
    bool showMemoryBarH = false;
    bool showMemoryGrid = false;
    bool showCassetteDeck = false;
    bool showHgrPaintEditor = false;
    bool showHgrSpriteEditor = false;
    bool showRewindBar = false;

    bool showDiskPanel = false;
    bool showDisk35Panel = false;
    bool showHdvPanel = false;
    bool showSmartPortPanel = false;
    bool showFujiNetPanel = false;
    bool showJoystickPanel = false;
    bool showChatMauvePanel = false;
    bool showSscPanel = false;
    bool showEthernetPanel = false;
    bool showNoSlotClockPanel = false;
    bool showPrinterPanel = false;
    bool showImageWriterPanel = true;
    bool showMockingboardPanel = false;
    bool showPhasorPanel = false;
    bool showEchoPlusPanel = false;
    bool showAudioMixer = false;
    bool showMouseInspector = false;

    bool showSlotConfigPanel = true;
    bool showMediaPanel = false;
    bool showRomStatusPanel = false;
    bool showAbstractionPanel = false;
    bool showFloppyEmu = false;
    bool showAiControlPanel = false;
    bool showDiskLibrary = true;

    bool showPasteFileDialog = false;
    bool showAbout = false;
    bool showWelcomePanel = false;
    bool showKeyboardPanel = false;

    // Panel-local working data. None of it is emulated machine state; panels
    // may eventually own these sub-states directly without changing the
    // MainWindow public ABI.
    bool hostCapsLock = false;
    std::vector<std::uint8_t> hgrPaintMemory;
    std::vector<std::uint8_t> hgrPaintAuxMemory;
    bool rewindHeldPrevious = false;

    bool printerBackPressure = false;
    std::string printerSavePath;
    std::string printerLastSaveStatus;
    ScreenDumpOptions printerDumpOptions;
    std::uint64_t printerArchivedSheets = 0;

    std::string mouseInspectorLogPath;
    std::unique_ptr<std::ofstream> mouseInspectorLogStream;
    double mouseInspectorLastLogTime = 0.0;

    bool floppyEmuFavoritesActive = false;
    std::string floppyEmuStatus;

    std::string tapeStatusMessage;
    double tapeStatusUntil = 0.0;
    double lastFrameTime = 0.0;

    std::string pasteDialogPath;
    bool pasteAutoUppercase = false;
    int diskDialogTargetSlot = -1;

    unsigned aboutImageTexture = 0;
    int aboutImageWidth = 0;
    int aboutImageHeight = 0;
    bool aboutImageTried = false;

    unsigned keyboardImageTexture = 0;
    int keyboardImageWidth = 0;
    int keyboardImageHeight = 0;
    bool keyboardImageTried = false;
    std::string keyboardImageError;

    bool kiosk = false;
    bool launchedInKiosk = false;
    int savedWindowX = 0;
    int savedWindowY = 0;
    int savedWindowWidth = -1;
    int savedWindowHeight = 0;
    bool savedWindowMaximized = false;

    bool kioskMenuOpen = false;
    int kioskActionSelection = 0;
    int kioskKeySelection = 0;
    int kioskRomDirectorySelection = 0;
    int kioskBrowseSelection = 0;
    std::vector<std::string> kioskDiskPaths;
    std::vector<std::string> kioskDiskLabels;
    int kioskDiskSelection = 0;
    std::string kioskStatus;
    std::string kioskMountedPath;
    std::vector<std::string> kioskRomDirectories;
    bool kioskRomDirectoriesLoaded = false;
    std::string kioskBrowseDirectory;
    std::vector<std::string> kioskBrowseSubdirectories;
    std::vector<std::string> kioskBrowseShortcutPaths;
    std::vector<std::string> kioskBrowseShortcutLabels;
    bool kioskNavigationHeld = false;
    double kioskNavigationNextTime = 0.0;
    bool kioskPausedByMenu = false;
    bool kioskPauseWasAlreadyStopped = false;
    bool kioskMenuWasOpen = false;
    bool kioskSwallowPad = false;
    int loggedJoystickHost = -2;
    bool loggedJoystickGamepad = false;
    std::array<bool, 4> padArrowHeld{};
    std::array<double, 4> padArrowNextTime{};

    // Host-side Mouse Card routing and capture. These values describe the
    // current ImGui/GLFW frame; none belongs to deterministic machine state.
    ImVec2 screenRectMin{0.0f, 0.0f};
    ImVec2 screenRectMax{0.0f, 0.0f};
    bool screenHovered = false;
    std::uint8_t mouseAppleX = 0;
    std::uint8_t mouseAppleY = 0;
    double lastMouseHostX = 0.0;
    double lastMouseHostY = 0.0;
    bool mouseInitialized = false;
    bool mouseButtonHeld = false;
    bool screenDraggingByTitleBar = false;
    double mouseSubAppleX = 0.0;
    double mouseSubAppleY = 0.0;
    bool mouseGrabbed = false;
    double mouseGrabHintUntil = 0.0;
};

} // namespace pom2

#endif // POM2_MAIN_WINDOW_UI_STATE_H
