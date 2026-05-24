// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_MAIN_WINDOW_H
#define POM2_MAIN_WINDOW_H

// MainWindow.h kept lean: only the headers strictly required by the
// public/private member types declared here. Card / panel / controller
// implementations are pulled in by MainWindow.cpp via include of their
// own headers. Changing a card or panel header recompiles its own TU
// (and MainWindow.cpp), not every TU that touches MainWindow.h.
//
// `M6502.h` stays because `M6502::CpuMode` appears in the
// `resolveCpuMode` signature below — nested enums can't be forward-
// declared. `SystemProfile` is reachable via an opaque enum-class
// forward decl, no SystemProfile.h needed here.

#include "M6502.h"
#include "Apple2Display.h"  // HiResMode (toolbar color/mono toggle remembers submode)

#include "imgui.h"  // ImU32 / ImVec2 used in struct MemRegion + member types

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

class Apple2Display;
class ClockCard;
class DiskIICard;
class EmulationController;
class JoystickInput;
class LeChatMauveCard;
class MockingboardCard;
class MouseCard;
class ProDOSHardDiskCard;
class SuperSerialCard;

namespace pom2 {
    class AiControlServer;
    class CassetteDeck_ImGui;
    class Disk35Controller_ImGui;
    class DiskController_ImGui;
    class DiskLibrary_ImGui;
    class CffaCard;
    class ProDOSBlockCard;
    class HdvController_ImGui;
    class JoystickPanel_ImGui;
    class LeChatMauve_ImGui;
    class Settings;
    class SmartPortCard;
    class SmartPort_ImGui;
    class Toolbar_ImGui;
    enum class SystemProfile;
    enum class CharRomLocale : uint8_t;
}
class MemoryViewer_ImGui;

class MainWindow
{
public:
    /// `forceIIPlus`: when true, skip the auto-probe for `roms/apple2e.rom`
    /// even if it is present and load `roms/apple2.rom` instead. Used by
    /// the `--ii-plus` CLI flag to fall back to II+ mode for software
    /// (e.g. Copy II Plus 8.3) that boots cleanly under the legacy ROM.
    explicit MainWindow(bool forceIIPlus = false);
    ~MainWindow();

    /// Apply a system profile via full cold-reset. Stops the CPU worker,
    /// wipes RAM + soft switches, reloads the profile's ROM + char ROM,
    /// re-plugs the slot cards (preserving currently-mounted disk/HDV
    /// paths so the user can boot the same media under a different
    /// Apple II model), flips the CPU type to the profile default
    /// (overridable via the `cpu_mode_override` setting), and lets the
    /// CPU restart from the new reset vector. Persists the chosen
    /// profile to settings so subsequent launches default to it.
    void applyProfile(pom2::SystemProfile p);
    pom2::SystemProfile currentProfile() const { return activeProfile; }

    /// Defined out-of-line in MainWindow.cpp — `controller` is a
    /// `unique_ptr<EmulationController>` here, so dereferencing it
    /// inline would require EmulationController.h. Keeping the body in
    /// the .cpp lets every translation unit that includes MainWindow.h
    /// stay clear of the EmulationController include cone.
    EmulationController& emul();
    Apple2Display&       displayRef();

    void setGlfwWindow(GLFWwindow* w);
    void render();

    // Hooks installed by main.cpp into GLFW callbacks.
    void onChar(unsigned int codepoint);
    void onKey (int key, int scancode, int action, int mods);
    /// GLFW cursor-position callback. Window-relative coordinates.
    /// Routes to the Apple II Mouse Card when the cursor is inside
    /// the Apple II Screen widget; otherwise no-op (ImGui handles it).
    void onMouseMove  (double x, double y);
    void onMouseButton(int button, int action);

    // HDV helper — cold-boot through the slot-5 ProDOS block-device ROM.
    // Public so main() can drive it from `POM2_AUTO_BOOT_HDV` for headless
    // trace capture without going through the File menu.
    void bootHdvImage();

    /// Mount `path` into the slot matching its type (5.25" Disk II / 800K
    /// 3.5" / ProDOS HDV, via `classifyDiskForSlot`) under the currently
    /// active profile + slot config, then cold-boot from it. Returns true
    /// on success; on failure `errOut` carries the reason. Drives both the
    /// Disk Library "insert + boot" buttons and the CLI positional-disk /
    /// `--kiosk` launcher. Must run on the UI thread (takes the state lock
    /// internally between frames).
    bool insertAndBootImage(const std::string& path, std::string& errOut);

    /// Kiosk mode: chrome-free full-screen — render() draws only the Apple
    /// II screen (no menu bar, toolbar, panels or dialogs). Set by main()
    /// from the `--kiosk` CLI flag before the first render.
    void setKioskMode(bool k) { kiosk_ = k; }
    bool kioskMode() const { return kiosk_; }

    // Apple II memory map region — used by the bar / grid widgets in
    // MainWindow_MemoryMaps.cpp. Public so file-local helpers in that TU
    // can take a `const std::vector<MemRegion>&`.
    struct MemRegion { uint16_t start, end; ImU32 color; const char* label; };

private:
    // Owning members held by unique_ptr so the include of each subsystem
    // header stays in MainWindow.cpp. Constructor / destructor defined
    // out-of-line for the same reason (unique_ptr<T> destruction needs
    // T to be a complete type).
    std::unique_ptr<EmulationController>          controller;
    std::unique_ptr<Apple2Display>                display;
    // Toolbar color↔mono toggle remembers the last mode on each side, so
    // round-tripping preserves the user's specific submode (e.g. Mono Green
    // ↔ Color 4-bit) rather than snapping to a single default each way.
    Apple2Display::HiResMode lastColorHiResMode_ = Apple2Display::HiResMode::ColorNTSC;
    Apple2Display::HiResMode lastMonoHiResMode_  = Apple2Display::HiResMode::MonoWhite;
    std::unique_ptr<MemoryViewer_ImGui>           memViewer;
    std::unique_ptr<pom2::Settings>               settings;
    std::unique_ptr<pom2::CassetteDeck_ImGui>     cassetteDeck;
    // One Disk II controller-panel per plugged DiskIICard (option C
    // 2026-05-15: DiskII can now appear in multiple slots, so the user
    // can wire DiskII slot 6 + DiskII slot 4 = 4 drives 5.25"). The
    // vector is rebuilt by `plugSlotsFromSettings`; index N corresponds
    // to `diskCards[N]`. `diskPanel` keeps a reference to the *primary*
    // (lowest-slot) panel so legacy menu wiring stays unchanged.
    std::vector<std::unique_ptr<pom2::DiskController_ImGui>> diskPanels;
    pom2::DiskController_ImGui*                              diskPanel = nullptr;
    std::unique_ptr<pom2::Disk35Controller_ImGui> disk35Panel;
    std::unique_ptr<pom2::DiskLibrary_ImGui>      diskLibrary;
    std::unique_ptr<pom2::HdvController_ImGui>    hdvPanel;
    std::unique_ptr<pom2::SmartPort_ImGui>        smartPortPanel;
    std::unique_ptr<pom2::JoystickPanel_ImGui>    joystickPanel;
    std::unique_ptr<pom2::LeChatMauve_ImGui>      chatMauvePanel;
    std::unique_ptr<pom2::Toolbar_ImGui>          toolbar;
    // All plugged Disk II cards, sorted by slot ascending. `diskCard`
    // (below) is the primary alias = `diskCards.empty() ? nullptr :
    // diskCards.front()`. Most legacy code paths use `diskCard` directly
    // (auto-turbo, AI control attach, menu Eject); the panel render loop
    // iterates `diskCards`.
    std::vector<DiskIICard*>     diskCards;
    DiskIICard*                  diskCard = nullptr;       // non-owning, owned by SlotBus
    ProDOSHardDiskCard*          hdvCard = nullptr;        // non-owning, owned by SlotBus
    pom2::CffaCard*              cffaCard = nullptr;       // non-owning, owned by SlotBus
    LeChatMauveCard*             chatMauveCard = nullptr;  // non-owning, owned by SlotBus
    SuperSerialCard*             sscCard = nullptr;        // non-owning, owned by SlotBus
    ClockCard*                   clockCard = nullptr;      // non-owning, owned by SlotBus
    MouseCard*                   mouseCard = nullptr;      // non-owning, owned by SlotBus
    MockingboardCard*            mockingboardCard = nullptr; // non-owning, owned by SlotBus
    pom2::SmartPortCard*         smartPortCard    = nullptr; // non-owning, owned by SlotBus
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

    std::unique_ptr<JoystickInput> joystick;
    GLFWwindow*                    window = nullptr;

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
    // Per-card disk panels (Disk II / Disk 3.5" / HDV) are off by
    // default since 2026-05-15 — the unified `Disk Library` panel
    // covers the 1-click insert+boot path for all three formats with
    // a single window. Users open the per-card panels on demand from
    // Devices menu when they need the deep state (track number, motor
    // LED, write-back checkbox, etc.).
    bool         showDiskPanel   = false;
    bool         showDisk35Panel = false;
    bool         showHdvPanel    = false;
    bool         showSmartPortPanel = false;
    bool         showJoystickPanel = false;
    bool         showChatMauvePanel = false;
    bool         showSscPanel       = false;
    // Mockingboard live state panel — shows VIA T1 / IFR / IER and the
    // two AY-3-8910 register banks. Primary use: diagnose silent
    // IRQ-driven music drivers (Ultima IV, Nox Archaist) by seeing
    // whether the music handler is actually writing AY registers.
    bool         showMockingboardPanel = false;
    // Audio mixer — master + per-channel sliders/mute (Speaker, Cassette,
    // Mockingboard, Disk 5.25", Disk 3.5"). Replaces the volume sliders
    // that used to live in the Status panel. Persisted as `show_mixer`.
    bool         showAudioMixer     = false;
    bool         showEmulationPanel = false;
    bool         showSlotConfigPanel = false;
    bool         showAiControlPanel  = false;
    // Toolbar pinned just below the menu bar. On by default — toggled
    // via Window → Toolbar; persisted as `show_toolbar`.
    bool         showToolbar         = true;
    // Unified disk browser (3-tab panel: 5.25/3.5/HDV). On by default
    // since 2026-05-15 — replaces the per-card library lists as the
    // primary way to browse + mount images. Toggled from Devices menu.
    // Persisted as `show_disk_library`.
    bool         showDiskLibrary     = true;
    // Initialised in the constructor body from SuperSerialCard::kDefaultPort
    // so we don't have to drag SuperSerialCard.h into this header.
    int          sscPortInput       = 0;

    // ── AI Control server (HTTP/1.1 on 127.0.0.1) ────────────────────────
    // Lifetime owned here; attach()'d after EmulationController is wired,
    // start()'d if the persisted setting was on at last shutdown. Exposed
    // via the Hardware menu's AI Control panel.
    std::unique_ptr<pom2::AiControlServer> aiServer;
    // Initialised in the constructor body from AiControlServer::kDefaultPort
    // for the same reason as sscPortInput above.
    int          aiPortInput   = 0;
    std::string  aiTokenInput;

    // Disk II insert dialog state moved to DiskController_ImGui (it
    // owns its own UX surface). MainWindow keeps only the ROM probe.
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
    // Mount-dialog state lives in `hdvPanel`.
    std::string hdvPath;
    std::string hdvStatus;

    // Cassette load/save dialog state moved to CassetteDeck_ImGui.
    std::string tapeStatusMessage;
    double      tapeStatusUntil = 0.0;
    double      lastFrameTime   = 0.0;

    // Paste-from-file dialog state.
    bool        showPasteFileDialog = false;
    std::string pasteDialogPath;
    bool        pasteAutoUppercase  = false;

    // Slot number of the DiskII the Insert-disk popup currently routes to.
    // Latched when any panel sets `insertDialogOpen` true; cleared when
    // the popup closes. Lets the popup survive panel pointer churn (rare
    // profile-switch races).
    int         diskDialogTargetSlot = -1;

    // Boot-time ROM probing.
    std::string romPath = "roms/apple2.rom";
    std::string charRomPath = "roms/apple2_char.rom";
    std::string romStatus = "no ROM loaded";

    // User-selected character-generator ROM locale. ProfileDefault means
    // "use the active profile's charRomProbeOrder"; anything else
    // overrides and bypasses the probe. Persisted as `char_rom_locale`
    // so the user's choice survives restarts and applies even before
    // the toolbar shows up. Default-initialised in the constructor
    // (header stays light — forward-declared enum can't be initialised
    // inline).
    pom2::CharRomLocale charRomLocale;

    // Active system profile. Tracked separately so the Presets menu can
    // mark the live entry with a checkmark and the title bar reflects
    // it. Initialised in the constructor body to AppleIIPlus (default
    // for II+ probe); user-driven Presets menu clicks + CLI --preset go
    // through `applyProfile()` which keeps this in sync.
    pom2::SystemProfile activeProfile;

    bool showAbout = false;

    // Kiosk mode (set by `--kiosk`): render() draws only the Apple II
    // screen, full-viewport, with no menu bar / toolbar / panels.
    bool kiosk_ = false;

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
    // Title-bar-only drag state for the Apple II Screen window. The
    // window is opened with `ImGuiWindowFlags_NoMove` so ImGui never
    // starts a window-move from the content area (clicks pass through
    // to the Mouse Card). When the user mouses-down on the title bar
    // we latch this flag and apply `io.MouseDelta` to the window pos
    // ourselves until the button is released.
    bool    screenDraggingByTitleBar = false;
    // Sub-tick accumulator: dx host pixels × (display.width()/widget_w)
    // = Apple-coord delta. We'd lose fractional motion if we truncated
    // each event, so carry the remainder across calls. Per-event delta
    // is clamped to ±127 (MCU's 8-bit signed wrap range) BEFORE we
    // subtract from the accumulator, so >127-tick events keep their
    // residual for the next event.
    double  mouseSubAppleX = 0.0;
    double  mouseSubAppleY = 0.0;

    /// Populate the SlotBus from `slot_1_card`..`slot_7_card` settings,
    /// instantiating each card with its slot number. Falls back to legacy
    /// defaults (DiskII=6, HDV=5, SSC=2, Clock=4, ChatMauve=7) when a
    /// slot key is absent. Validates uniqueness — duplicate card-type
    /// requests log a warning and skip the second instance. Populates the
    /// raw `*Card` pointer fields and the `slotCards[]` index.
    void plugSlotsFromSettings();

    // ── Disk insert+boot routing (shared by Disk Library UI + CLI) ───────
    // Promoted from file-local lambdas in renderDiskLibraryWindow so
    // insertAndBootImage() can reuse the exact same routing. Each takes
    // the state lock internally.
    /// Route a 3.5" image to the //c+ on-board hub or a SmartPort card
    /// unit `driveIdx`, auto-creating a SmartPort35Unit if needed.
    bool routeMount35 (int driveIdx, const std::string& path, std::string& errOut);
    /// Route an HDV image to the ProDOSHardDiskCard or a SmartPort card's
    /// unit 0, auto-creating a SmartPortHdvUnit if needed. `bootSlotOut`
    /// receives the slot to boot from.
    bool routeMountHdv(const std::string& path, int& bootSlotOut, std::string& errOut);
    /// The active HDV-class block device for the HDV Library / turbo / eject —
    /// prefers the MAME-faithful CffaCard when plugged, else the synthetic
    /// ProDOSHardDiskCard. nullptr when neither is present.
    /// NOTE: this is the *primary* (single-target) accessor kept for the
    /// legacy menu/library paths; for anything that must touch EVERY block
    /// card (eject-all, auto-turbo, the Slot Manager) enumerate via
    /// `blockCards()` so a second block card isn't silently ignored.
    pom2::ProDOSBlockCard* hdvDevice() const;
    /// Enumerate ALL plugged ProDOS block-device cards (synthetic HDV +
    /// MAME-faithful CFFA), sorted by slot ascending, by walking the
    /// SlotBus. The bus — not our raw `*Card` pointers — is the source of
    /// truth, so this stays correct when several block cards coexist.
    std::vector<pom2::ProDOSBlockCard*> blockCards() const;
    /// Enumerate ALL plugged SmartPort (Liron-class) slot cards, sorted by
    /// slot ascending. Same rationale as `blockCards()`.
    std::vector<pom2::SmartPortCard*> smartPortCards() const;
    /// Ensure the config can host an HDV image: returns the slot of an
    /// existing HDV or SmartPort card, or plugs a fresh ProDOSHardDiskCard
    /// into a free slot (preferring slot 7) and returns that. Used by the
    /// CLI/kiosk launcher so `POM2 game.hdv` boots even when the saved slot
    /// config has only Disk II cards. Returns -1 if no free slot exists.
    /// NOT persisted — the saved slot configuration is left untouched.
    int  ensureHdvCardForBoot();

    void renderMenuBar();
    void renderScreenWindow();
    /// Draw the Apple II framebuffer texture into the current ImGui
    /// window's content region, scaled + centred (letterboxed) to preserve
    /// the 4:3 aspect. Shared by the normal screen window and the kiosk
    /// full-viewport path. Updates screenRectMin/Max for Mouse Card routing.
    void drawScreenImage();
    /// Kiosk render path: a single borderless full-viewport window showing
    /// only the screen. No menu bar / toolbar / panels.
    void renderKiosk();
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
    void updateAutoTurbo();
    void renderDiskPanelWindow();
    void renderDiskFileDialog();
    void renderDiskLibraryWindow();
    void renderDisk35PanelWindow();
    void renderDisk35FileDialog();
    void renderHdvPanelWindow();
    void renderHdvFileDialog();
    void renderSmartPortPanelWindow();
    void renderChatMauvePanelWindow();
    void renderMockingboardPanelWindow();
    void renderSscPanelWindow();
    void renderJoystickPanelWindow();
    void renderAudioMixerWindow();
    void renderAiControlPanelWindow();
    void pollJoystickAndPushToMemory();
    void renderAboutDialog();
    /// Slot Configuration panel. Implemented in MainWindow_Slots.cpp.
    void renderSlotConfigPanel();
    /// Tear down the SlotBus and re-run plugSlotsFromSettings(). Called
    /// by the Slot Configuration panel's Apply button. Stops the
    /// emulation worker around the rebuild so card destructors / new
    /// constructors don't race against a running CPU thread.
    void restartEmulationFromSettings();

    /// Pick the first existing file path in `candidates`. Empty string
    /// when none exists. Used by applyProfile to probe ROM candidates.
    static std::string firstExistingPath(const std::vector<std::string>& candidates);

    /// Resolve the CPU mode to use given the profile default + the
    /// `cpu_mode_override` setting. `auto` (default) → profile.defaultCpu;
    /// `nmos` → NMOS; `65c02` → CMOS. Logged to console on each apply.
    M6502::CpuMode resolveCpuMode(M6502::CpuMode profileDefault) const;

    /// Pick the motor-sound pitch multiplier for a profile. //c / //c+
    /// use a Sony internal 5.25" drive that spins up faster and at a
    /// higher pitch than the original Shugart-based Disk II — bumping
    /// the motor pitch on those profiles approximates that without
    /// dragging in a second sample set. All other profiles → 1.0
    /// (native MAME Disk II samples).
    static float floppyMotorPitchForProfile(pom2::SystemProfile p);

    // Paste helpers — feed text into the keyboard buffer.
    void pasteFromClipboard();
    void pasteFromFile(const std::string& path);

    // Write the current display framebuffer to ./screenshot_NNN.ppm. The
    // sequence number auto-increments to avoid overwriting prior captures
    // within the same session.
    void saveScreenshot();

    // Eject every loaded image (Disk II, HDV, SmartPort units, 3.5").
    // Shared by the Disk Library header-row button.
    void ejectAllDisks();

    void uploadScreenTexture();

    // Translate a GLFW key/codepoint into an ASCII byte and feed it to
    // Memory::queueKey(). Memory's strobe handling is hardware-faithful;
    // we just hand it the character.
    void injectAscii(uint8_t ascii);
};

#endif // POM2_MAIN_WINDOW_H
