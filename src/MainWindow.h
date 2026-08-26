// VERHILLE Arnaud 2026

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
#include "Mat4.h"           // pom2::OrbitCamera member (3D voxel view)
#include "MouseGrab.h"      // pom2::mousegrab::Context (mouseGrabContext)
#include "Pom2Theme.h"      // pom2::UiAccent member (View ▸ Interface)

#include "imgui.h"  // ImU32 / ImVec2 used in struct MemRegion + member types

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

class Apple2Display;
class AudioSource;
class DiskIICard;
class EmulationController;
// The state-lock handle (EmulationController.h). Forward-declared rather
// than included for the same reason as EmulationController itself — this
// header stays outside that include cone; see `emul()` below.
namespace pom2 { class StateAccess; }
class JoystickInput;
class LeChatMauveCard;
class ProDOSHardDiskCard;
class SlotPeripheral;

namespace pom2 {
    class AudioCoordinator;
    class DebugCoordinator;
    class DevicePanelCoordinator;
    class MouseCoordinator;
    struct MainWindowUiState;
    class NetworkCoordinator;
    class PrinterCoordinator;
    class SlotCardFactory;
    class SlotConfigurationCoordinator;
    class SlotProvisioningCoordinator;
    class SlotRebuildCoordinator;
    class StorageCoordinator;
    class AiControlServer;
    class CassetteDeck_ImGui;
    class Rewind_ImGui;
    class Disk35Controller_ImGui;
    class NtscPostProcessor;
    class CrtEffectStack;
    class Voxel3DRenderer;
    struct NtscParams;
    class DiskController_ImGui;
    class DiskLibrary_ImGui;
    class ProDOSBlockCard;
    class HdvController_ImGui;
    class JoystickPanel_ImGui;
    class LeChatMauve_ImGui;
    class Settings;
    class SmartPortCard;
    class SmartPort_ImGui;
    class PrinterSoundDevice;
    class PrinterHistory;
    class FujiNet_ImGui;
    class FloppyEmuDevice;
    class FloppyEmu_ImGui;
    class ImageWriter;
    class ImageWriter_ImGui;
    class UthernetCard;
    class UthernetIICard;
    class FujiNetHost;
    class Uthernet_ImGui;
    class Toolbar_ImGui;
    class CommandPalette_ImGui;
    class RomStatus_ImGui;
    class AbstractionLevels_ImGui;
    class Keyboard_ImGui;
    enum class SystemProfile;
    enum class CharRomLocale : uint8_t;
}
class MemoryViewer_ImGui;
class Pom2HgrPaintHost;
namespace hgrpaint { class HgrPaintEditor; }
namespace hgrsprite { class HgrSpriteEditor; }

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

    /// Read/write the Le Chat Mauve "Dragon Wars compatibility" toggle
    /// from main()'s Phase-B CLI handler. No-ops (returns false) when no
    /// LeChatMauveCard is plugged at boot. Persists to Settings so the
    /// next session picks it up automatically.
    bool setChatMauveInvertBit7(bool v);

    void setGlfwWindow(GLFWwindow* w);

    /// Monitor content scale (`glfwGetWindowContentScale`), 1.0 on a
    /// non-HiDPI display. Set once by main() after the ctor has restored
    /// `ui_accent` / `ui_scale` from Settings; re-applies the theme so the
    /// persisted zoom lands on top of the display scale.
    void setDpiScale(float s);

    /// Host caps-lock LOCK state, latched from the GLFW modifier bits (see
    /// glfw_key_callback). Surfaced as a status-bar badge.
    void setHostCapsLock(bool on);

    void render();

    // Hooks installed by main.cpp into GLFW callbacks.
    void onChar(unsigned int codepoint);
    void onKey (int key, int scancode, int action, int mods);
    /// GLFW cursor-position callback. Window-relative coordinates.
    /// Routes to the Apple II Mouse Card when the cursor is inside
    /// the Apple II Screen widget; otherwise no-op (ImGui handles it).
    void onMouseMove  (double x, double y);
    void onMouseButton(int button, int action);
    /// GLFW window-focus callback. Releases a captured pointer when the
    /// window loses focus — a grab that survives Alt-Tab would keep
    /// swallowing the desktop's pointer with no visible owner.
    void onWindowFocus(bool focused);

    /// Host-pointer capture for the Mouse Card. `setMouseGrab(true)` puts
    /// GLFW in GLFW_CURSOR_DISABLED (unbounded relative deltas, OS cursor
    /// hidden) and takes the mouse away from ImGui; false restores both.
    /// Refused with a status message when no Mouse Card is plugged.
    /// Release: Ctrl+Alt+G, middle click, focus loss, or card unplug.
    void setMouseGrab(bool on);
    void toggleMouseGrab();
    bool mouseGrabbed() const;

    /// GLFW file-drop callback (installed by main.cpp). Routes the first
    /// recognised disk image among `paths` through `insertAndBootImage`
    /// (auto-routes Disk II / SmartPort 3.5" / ProDOS HDV). Unrecognised
    /// or extra files are ignored; a status-bar message reports the result.
    void onFileDrop(int count, const char** paths);

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

    /// CLI (`--fujinet` / `--fujinet-serial`): plug a FujiNet relay card into
    /// `slot` and arm its link before the machine boots, so an autostart scan
    /// finds a FujiNet on its first pass. `serialDevice` empty in serial mode
    /// means "auto-pick when exactly one candidate exists".
    ///
    /// `slot` is IN/OUT. When `slotExplicit` is false it is only a preference:
    /// slot 7's first-run default is the Le Chat Mauve, so a bare `--fujinet`
    /// would otherwise always be refused for a slot the user never chose. In
    /// that case POM2 falls back to the first free slot (7→1) and writes it
    /// back. An EXPLICIT `--fujinet-slot N` that is occupied stays an error:
    /// destroying a live card would race the CPU worker, and silently evicting
    /// the user's Disk II would be worse than an error message.
    ///
    /// The request is REMEMBERED, so `plugSlotsFromSettings()` can re-apply it
    /// after every slot rebuild. Without that a `--preset` on the same command
    /// line destroyed the card moments after this returned true: applyProfile
    /// clears the SlotBus and re-seeds strictly from the `slot_N_card` keys,
    /// which a one-shot CLI card deliberately never writes.
    bool plugFujiNetFromCli(int& slot, bool slotExplicit, bool serial,
                            const std::string& serialDevice,
                            int tcpPort, std::string& errOut);

#ifdef __EMSCRIPTEN__
    /// Browser UX: toolbar reset should relaunch the boot image that was
    /// passed in through the WASM page arguments (Total Replay by default).
    void setBrowserResetBootImage(const std::string& path) { browserResetBootImage_ = path; }
#endif

    /// Kiosk mode: chrome-free full-screen — render() draws only the Apple
    /// II screen (no menu bar, toolbar, panels or dialogs). Set by main()
    /// from the `--kiosk` CLI flag before the first render.
    /// Set the kiosk flag WITHOUT touching the window (startup path —
    /// main() has already created the window full-screen when `--kiosk`
    /// was passed).
    /// Startup path: sets BOTH the live flag and `launchedInKiosk_`, and
    /// puts Settings into read-only mode (central suppression — a
    /// `--kiosk` session must never write state.cfg from ANY of the ~20
    /// UI save sites, not just the few that remember to check).
    /// Out-of-line: Settings is only forward-declared here.
    void setKioskMode(bool k);
    bool kioskMode() const;
    /// Whether settings writes are suppressed. True while in kiosk, and
    /// for the WHOLE session when launched with `--kiosk` (see
    /// `launchedInKiosk_`) — toggling to the GUI to look around must not
    /// silently start rewriting the user's state.cfg.
    bool settingsReadOnly() const;

    /// Runtime GUI ↔ kiosk transition: flips the flag AND moves the GLFW
    /// window between exclusive full-screen and its previous windowed
    /// geometry. The emulated machine is NOT touched — kiosk is purely a
    /// windowing + render-path + settings-write-suppression mode, so the
    /// switch is instant and loses no state (no snapshot round-trip
    /// needed). Safe to call with no window bound (headless tests): it
    /// then only flips the flag. Returns the new state.
    bool toggleKioskMode();
    /// Explicit form of the above.
    void setKioskModeRuntime(bool k);

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
    std::unique_ptr<pom2::Settings>               settings;
    std::unique_ptr<pom2::DevicePanelCoordinator> devicePanelCoordinator_;
    std::unique_ptr<pom2::MouseCoordinator>       mouseCoordinator_;
    std::unique_ptr<pom2::MainWindowUiState>      uiState_;
    std::unique_ptr<pom2::AudioCoordinator>       audioCoordinator_;
    std::unique_ptr<pom2::DebugCoordinator>       debugCoordinator_;
    std::unique_ptr<pom2::NetworkCoordinator>     networkCoordinator_;
    std::unique_ptr<pom2::PrinterCoordinator>     printerCoordinator_;
    std::unique_ptr<pom2::SlotCardFactory>        slotCardFactory_;
    std::unique_ptr<pom2::SlotConfigurationCoordinator> slotCoordinator_;
    std::unique_ptr<pom2::StorageCoordinator>     storageCoordinator_;
    std::unique_ptr<pom2::SlotProvisioningCoordinator>
                                                slotProvisioningCoordinator_;
    std::unique_ptr<pom2::CassetteDeck_ImGui>     cassetteDeck;
    std::unique_ptr<pom2::Rewind_ImGui>           rewindPanel_;
    // One Disk II controller-panel per plugged DiskIICard (option C
    // 2026-05-15: DiskII can now appear in multiple slots, so the user
    // can wire DiskII slot 6 + DiskII slot 4 = 4 drives 5.25"). The
    // vector is rebuilt by `plugSlotsFromSettings`; index N corresponds
    // to StorageCoordinator's slot-sorted Disk II inventory. `diskPanel`
    // keeps a reference to the *primary*
    // (lowest-slot) panel so legacy menu wiring stays unchanged.
    std::vector<std::unique_ptr<pom2::DiskController_ImGui>> diskPanels;
    pom2::DiskController_ImGui*                              diskPanel = nullptr;
    std::unique_ptr<pom2::Disk35Controller_ImGui> disk35Panel;
    std::unique_ptr<pom2::DiskLibrary_ImGui>      diskLibrary;
    // Ctrl+Shift+P fuzzy launcher over every menu item, panel toggle and
    // machine action. See CommandPalette_ImGui.h for why it exists (42 menu
    // items, ~33 panels, 4 keyboard shortcuts).
    std::unique_ptr<pom2::CommandPalette_ImGui>   cmdPalette;
    std::unique_ptr<pom2::HdvController_ImGui>    hdvPanel;
    std::unique_ptr<pom2::SmartPort_ImGui>        smartPortPanel;
    std::unique_ptr<pom2::FujiNet_ImGui>          fujiNetPanel;
    // BMOW Floppy Emu: the device model (mode/SD/favorites) + its OLED panel.
    std::unique_ptr<pom2::FloppyEmuDevice>        floppyEmu;
    std::unique_ptr<pom2::FloppyEmu_ImGui>        floppyEmuPanel;
    std::unique_ptr<pom2::JoystickPanel_ImGui>    joystickPanel;
    // Apple ImageWriter II: the host-side printer (page raster +
    // control-language interpreter) and its paper-tray window. The
    // printer is downstream of whatever printer *interface* card is
    // plugged — `pumpImageWriter()` streams that card's spool into it.
    /// Declared BEFORE imageWriter: the ctor initialiser list runs in
    /// declaration order, and imageWriter is handed this pointer there.
    std::unique_ptr<pom2::PrinterSoundDevice>     printerSound;
    /// Durable printouts. Every ejected sheet is written here, so a printout
    /// outlives the session — the in-ImageWriter stack is capped at 32 and
    /// dies with the process.
    std::unique_ptr<pom2::PrinterHistory>         printerHistory;
    std::unique_ptr<pom2::ImageWriter>            imageWriter;
    std::unique_ptr<pom2::ImageWriter_ImGui>      imageWriterPanel;
    std::unique_ptr<pom2::LeChatMauve_ImGui>      chatMauvePanel;
    std::unique_ptr<pom2::Toolbar_ImGui>          toolbar;
    // Portable HGR Paint editor (src/hgrpaint/, shared verbatim with POM1)
    // + its POM2 host seam (pokes / offscreen NTSC render / file I/O).
    std::unique_ptr<Pom2HgrPaintHost>             hgrPaintHost;
    std::unique_ptr<hgrpaint::HgrPaintEditor>     hgrPaintEditor;
    // Sprite editor (src/hgrsprite/) — reuses the same host seam.
    std::unique_ptr<hgrsprite::HgrSpriteEditor>   hgrSpriteEditor;
    /// Status of the Mouse Card ROM probe — used by the Slot
    /// Configuration UI to indicate whether 'mouse' is selectable.
    /// "" = not yet probed, "loaded: <paths>" = ready, otherwise the
    /// failure message.
    std::string                  mouseRomStatus;

    std::unique_ptr<JoystickInput> joystick;
    GLFWwindow*                    window = nullptr;

    unsigned int screenTexture = 0;     // GL texture name (lazy)
    int          screenTextureWidth  = 0;
    int          screenTextureHeight = 0;
    // OpenEmulator-style composite NTSC shader pipeline. Lazily
    // initialised on the first frame the user selects ColorCompositeOE;
    // stays alive across mode toggles so persistence state isn't lost
    // when the user briefly flips to another mode and back.
    std::unique_ptr<pom2::NtscPostProcessor> ntscFx;
    // Universal CRT effect stack (Phase 3): applies the same scanline /
    // shadow-mask / barrel / persistence / BCS layers to the framebuffer of
    // ANY colour mode (not just ColorCompositeOE). Opt-in via the menu;
    // shares the NtscParams driven by the CRT Settings panel. Lazily
    // initialised. Always runs for non-OE pipelines (OE bakes its own
    // effects in its demod shader).
    std::unique_ptr<pom2::CrtEffectStack> crtFx;
    // 3D voxel view (MicroM8 "Voxel Cube"): rebuilds the presented framebuffer
    // as an upright 4:3 slab of equal-depth instanced cubes, viewed by an orbit
    // camera (left-drag / wheel). Orthogonal to the colour pipeline — works on
    // any HiResMode. Lazily initialised; off (persisted under `show_3d_voxel`).
    std::unique_ptr<pom2::Voxel3DRenderer> voxel3d_;
    pom2::OrbitCamera voxelCam_;
    // Master ON/OFF for the whole CRT effect stack (the button at the top of
    // the CRT Settings window). When off, every pipeline presents its raw
    // framebuffer — the colour demod still runs, only the CRT glass is
    // bypassed. Persisted under `crt_effects_enabled`.
    bool         crtEffectsEnabled = true;

    // Presentation aspect (Phase 6). The Apple II's 280×192 active area was
    // shown on a 4:3 CRT → its pixels are NOT square. Square = 1:1 logical
    // pixels (crisp emulator default); Crt43 = true 4:3 monitor shape;
    // Integer = square snapped to an integer multiple (no scaling shimmer).
    enum class AspectMode { Square, Crt43, Integer };
    AspectMode   aspectMode = AspectMode::Square;

    // ── Docking layout (View ▸ Layout) ───────────────────────────────────
    // POM2 hosts a DockSpace over the viewport work area (below the menu bar
    // + toolbar, above the status bar) so its ~33 panels become tabs in a
    // persistent layout instead of a pile of overlapping windows. ImGui saves
    // the docks into `imgui.ini` by itself; we only seed a curated starting
    // layout and offer task-oriented presets.
    //
    // Presets dock by *literal window title*, so only panels whose title is a
    // fixed string can be placed. Slot-numbered panels (Disk II, 3.5", HDV,
    // SmartPort, Printer) build their title at runtime and are left floating
    // on first open — the user can dock them once and ImGui remembers.
    enum class DockLayout {
        Reset,        ///< Screen + Disk Library + an inspector tab group.
        Emulation,    ///< Screen wide, storage panels right. No debug tools.
        Debug,        ///< Screen left, memory/CPU inspectors right + bottom.
        Audio,        ///< Screen left, Mockingboard/Phasor/mixer right.
    };
    ImGuiID    dockspaceId_        = 0;
    DockLayout pendingDockLayout_  = DockLayout::Reset;
    bool       dockLayoutRequested_= false;
    /// True once a layout has been seeded into `imgui.ini` (persisted as
    /// `ui_dock_seeded`). Without it, every launch would rebuild the default
    /// layout and throw away whatever the user had docked.
    bool       dockSeeded_         = false;
    // ── Disk Library favourites / recents ────────────────────────────────
    // Host-owned because the panel has no Settings access. Persisted as
    // newline-free, '\n'-unsafe paths joined by '\x1f' (unit separator) under
    // `library_favourites` / `library_recents` — state.cfg is a flat
    // key=value file, and a disk path can legitimately contain spaces,
    // commas and semicolons, so the separator has to be something a
    // filesystem path cannot.
    std::vector<std::string> libraryFavourites_;
    std::vector<std::string> libraryRecents_;
    bool                     libraryHideSizeDate_ = false;
    static constexpr std::size_t kMaxLibraryRecents = 12;
    /// Move `path` to the front of the recents list, de-duplicating and
    /// trimming to kMaxLibraryRecents.
    void noteLibraryRecent(const std::string& path);

    void renderDockSpace();
    /// Build the command list, draw the palette, dispatch the pick.
    void renderCommandPalette();
    /// Execute a palette command by id. Single dispatch point — the
    /// palette itself has no idea what any command does.
    void runCommand(const std::string& id);
    /// Open the palette (Ctrl+Shift+P, or Tools â¸ Command palette).
    void openCommandPalette();
    void applyDockLayout(DockLayout preset);

    // ── Interface appearance (View ▸ Interface) ──────────────────────────
    // Accent hue + user zoom, both persisted (`ui_accent` / `ui_scale`).
    // `dpiScale_` is the monitor content scale supplied by main(); the
    // effective geometry scale is uiScale_ × dpiScale_. Changing any of the
    // three calls `applyUiTheme()`, which rebuilds the style from scratch —
    // ScaleAllSizes is cumulative, so incremental re-application would
    // compound (see Pom2Theme.h).
    pom2::UiAccent uiAccent_ = pom2::UiAccent::Amber;
    float          uiScale_  = 1.0f;
    float          dpiScale_ = 1.0f;
    void applyUiTheme();

    // Panels which are created lazily or do not already live beside the
    // main composition objects above. Their mutable working data is held by
    // the opaque MainWindowUiState rather than expanding this public header.
    std::unique_ptr<pom2::Uthernet_ImGui> ethernetPanel;
    std::unique_ptr<pom2::RomStatus_ImGui> romStatusPanel;
    std::unique_ptr<pom2::AbstractionLevels_ImGui> abstractionPanel;
    // Initialised in the constructor body from SuperSerialCard::kDefaultPort
    // so we don't have to drag SuperSerialCard.h into this header.
    int          sscPortInput       = 0;

    // ── AI Control server (HTTP/1.1 on 127.0.0.1) ────────────────────────
    // Lifetime owned here; attach()'d after EmulationController is wired,
    // start()'d if the persisted setting was on at last shutdown. Exposed
    // via the Hardware menu's AI Control panel.
    std::unique_ptr<pom2::AiControlServer> aiServer;
    // Owns the ordered topology transaction shared by profile switches and
    // Slot Configuration Apply. Declared after every callback dependency so
    // it is destroyed first and never retains a callable into a dead owner.
    std::unique_ptr<pom2::SlotRebuildCoordinator> slotRebuildCoordinator_;
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
    // of the read drops it to <1 s. Off by default — preserves authentic
    // speed and the floppy mechanical sounds (the turbo collapses
    // wall-clock to zero, which mutes the seek/spindle audio).
    bool        diskTurboWhileMotor = false;
    // Speed to restore when turbo disengages. NTSC 1× at construction;
    // applyProfile re-seeds it with the profile's defaultCyclesPerFrame
    // (20313 PAL, 68180 //c+) so the restore never underclocks.
    int         diskSavedCyclesPerFrame = 17045;
    bool        diskTurboActive = false;

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

    // Welcome / Quick Start panel. Opened from Help → Welcome, and
    // auto-opened on first launch when no main ROM was found so a
    // newcomer sees where to drop firmware/disks instead of a bare
    // "NO ROM" screen. `romLoaded_` mirrors the last ROM-load result so
    // the panel can show the no-ROM guidance prominently.
    bool romLoaded_       = false;
    // Lazy panel object; its texture/cache working state lives in uiState_.
    std::unique_ptr<pom2::Keyboard_ImGui> keyboardPanel;

    // Kiosk flags and saved window geometry live in uiState_.
    /// Persist the current windowed geometry into Settings (`window_x/y/w/h`,
    /// `window_maximized`) and restore it back into the members. Without
    /// this the geometry lived only in memory, so there was nothing to
    /// restore after a quit-from-kiosk, and a `--kiosk` launch that toggled
    /// to the GUI fell back to a hard-coded default size.
    void saveWindowGeometryToSettings();
    bool loadWindowGeometryFromSettings();

public:
    /// Measure the live window and fold it into Settings. MUST be called
    /// while GLFW is still initialised — ~MainWindow runs after
    /// glfwTerminate(), where every glfwGetWindow* call is a no-op that
    /// zeroes its out-params. Called from main() just before teardown.
    /// No-op in kiosk (the live geometry is full-screen) and when the
    /// session is settings-read-only.
    void captureWindowGeometryNow();

private:

    // ── Kiosk in-game menu (gamepad-driven, keyboard-free) ──────────────
    // An overlay exclusive to kiosk mode. Two entry points:
    //   • START (or F1) → the two-zone Start menu: a GAMES list (the disk
    //     images next to the booted disk + any extra ROM folders) and an
    //     ACTIONS column (Restart / Keyboard / ROM folders / Quit). LEFT/
    //     RIGHT swaps focus between the two zones, UP/DOWN moves within the
    //     focused zone, FIRE validates it. The machine is PAUSED while this
    //     is up (like inserting a disk on a real machine at rest).
    //   • SELECT (or K) → the Keyboard band: a 2D grid of Apple II keys sent
    //     live to the running game (NOT paused) via Memory::queueKey.
    // From the Start menu, "ROM folders" opens a gamepad directory browser
    // to add/remove extra scan folders (persisted outside state.cfg so the
    // kiosk's read-only main config is never touched).
    enum class KioskPage { List, Keys, Quit, Browse, RomDirs };
    enum class KioskZone { Games, Actions };
    static constexpr int kKioskActionCount = 5;   // Restart/Keyboard/ROMs/ExitKiosk/Quit

    KioskPage   kioskPage_      = KioskPage::List;
    KioskZone   kioskZone_      = KioskZone::Games;
    // Mutable menu contents, cursors, repeat timers, pause bookkeeping and
    // joystick diagnostics are frontend-only working state in uiState_.

    /// Poll gamepad/keyboard and drive the whole kiosk menu state machine
    /// (open/close, page/zone navigation, activation). Once per frame in
    /// kiosk mode. Also keeps the pause state in sync with the active page.
    void updateKioskMenu();
    /// Draw the active kiosk page (list / keyboard / quit / browse / romdirs)
    /// when the menu is open.
    void renderKioskMenu();

    /// Open the Start menu on the GAMES zone and rescan the disk list.
    void openKioskStartMenu();
    /// (Re)build kioskDiskPaths_ from the booted disk's folder + extra ROM
    /// folders, sorted by name-proximity to the mounted disk. Also called on
    /// the RomDirs → List transition so folder edits show up immediately.
    /// No-op-safe with no Disk II / no folder.
    void kioskRescanDisks();
    /// Swap the highlighted disk into the boot Disk II drive without reboot.
    void kioskMountSelected();
    /// Validate the focused zone's item (mount a disk, or run an action).
    void kioskActivateFocused();
    /// Send the highlighted key-grid cell to the running machine (live).
    void kioskInjectSelectedKey();
    /// The Disk II card the kiosk selector targets: slot 6 if present, else
    /// the primary card. nullptr when the config has no Disk II at all.
    DiskIICard* kioskBootDiskCard();

    // Pause helper: park (Mode::Stopped) / resume (Mode::Running) the worker
    // only when we're the one that paused it. `want==true` → paused.
    void kioskSetPaused(bool want);

    // ── ROM-folders manager + directory browser helpers ────────────────
    void kioskScanBrowse(const std::string& dir);     // fill subdirs for `dir`
    void kioskComputeShortcuts();                      // /, Home, mounts
    void kioskLoadRomDirs();                            // read persisted list
    void kioskSaveRomDirs();                            // write persisted list
    bool kioskPruneRomDirs();                          // drop vanished folders

#ifdef __EMSCRIPTEN__
    std::string browserResetBootImage_;
#endif

    // ── Mouse Card host-input plumbing (Phase 5) ─────────────────────
    // All mutable geometry/capture/input fields described below are held by
    // the opaque uiState_; MainWindow retains only the routing operations.
    // Apple II Screen widget rect, window-relative. Updated every
    // frame by `renderScreenWindow()` so the GLFW cursor-pos callback
    // (which fires async to the render loop) can map a host position
    // onto Apple pixels. Geometry only — see `screenHovered_` for who
    // *owns* the pointer.
    // ImGui's z-order aware verdict on the screen widget, captured next
    // to the Image in `renderScreenWindow()` and cleared at the top of
    // every `render()` so a collapsed / hidden / never-drawn screen
    // window cannot leave a stale `true` behind.
    //
    // This — not the rect — is what `mouseGrabContext()` feeds to
    // `mousegrab::Context::screenHovered`. A rect containment test cannot
    // see an open dropdown, popup or docked panel drawn over the screen:
    // they all sit inside the rect, so clicks aimed at them were reaching
    // the guest *and* arming the click-to-grab capture underneath.
    // Running 8-bit Apple II "mouse units" counter — mirrors MAME's
    // IPT_MOUSE_X/Y (0..0xFF wrapping). The MCU firmware computes
    // deltas with 8-bit subtraction.
    // Last GLFW cursor position; used to compute per-frame deltas.
    // Title-bar-only drag state for the Apple II Screen window. The
    // window is opened with `ImGuiWindowFlags_NoMove` so ImGui never
    // starts a window-move from the content area (clicks pass through
    // to the Mouse Card). When the user mouses-down on the title bar
    // we latch this flag and apply `io.MouseDelta` to the window pos
    // ourselves until the button is released.
    // Sub-tick accumulator: dx host pixels × (display.width()/widget_w)
    // = Apple-coord delta. We'd lose fractional motion if we truncated
    // each event, so carry the remainder across calls. Per-event delta
    // is clamped to ±127 (MCU's 8-bit signed wrap range) BEFORE we
    // subtract from the accumulator, so >127-tick events keep their
    // residual for the next event.

    // ── Absolute closed-loop cursor sync (host ⇄ guest) ────────────────
    // The Mouse Card is a purely *relative* quadrature device, so a host
    // delta fed open-loop drifts away from the guest cursor in absolute
    // terms (clamp-edge losses + scale mismatch). When the AppleMouse
    // firmware is active (mode byte $07F8+s bit 0) we instead drive the
    // guest *toward* the host cursor's absolute position-in-widget, read
    // back from the firmware screen holes ($0478+s/$0578+s low/high X,
    // $04F8+s/$05F8+s low/high Y — layout per the Apple II Mouse FAQ).
    // To avoid quadrature windup (host cursor events fire far faster than
    // the app polls READMOUSE, so the holes are stale between polls) we
    // edge-trigger: a correction is injected only once the holes have
    // moved, i.e. once the app has consumed the previous batch. Sentinel
    // -1 forces the first correction.

    // ── Host-pointer capture ("mouse grab") ───────────────────────────
    // Policy in `MouseGrab.h`; this is the runtime state it reads. Like
    // kiosk, a grab is a pure host-side mode: the machine never sees it, so
    // nothing about it is snapshotted. There is no click-to-grab preference
    // any more — a left click never captures, so the setting it used to
    // gate (`mouse_click_to_grab`) could no longer change anything, and a
    // preference that does nothing is worse than no preference. Stale keys
    // left in an existing state.cfg are simply never read.
    /// `lastFrameTime` deadline for the status bar's spelled-out "how to
    /// get out" hint beside the GRAB chip. Nothing is drawn over the
    /// emulated screen any more.

    /// Screen-overlay captions for the capture contract ("click to capture"
    /// / "Ctrl+Alt+G to release"). Called from `drawScreenImage`, so both
    /// the windowed and the kiosk path get them.

    /// Snapshot the host/UI state the grab policy decides on (card plugged,
    /// captured, cursor-in-screen-rect, voxel view, preference). Uses
    /// `lastMouseHostX/Y` + `screenRectMin/Max`, both refreshed before any
    /// caller needs them (cursor callback / previous frame's render).
    pom2::mousegrab::Context mouseGrabContext() const;

    /// Populate the SlotBus from `slot_1_card`..`slot_7_card` settings,
    /// instantiating each card with its slot number. Falls back to legacy
    /// defaults (DiskII=6, HDV=5, SSC=2, Clock=4, ChatMauve=7) when a
    /// slot key is absent. Validates uniqueness — duplicate card-type
    /// requests log a warning and skip the second instance. Populates the
    /// The resolved configuration plan stays separate from the live SlotBus
    /// topology; construction failures never rewrite the user's plan.
    ///
    /// **The caller owns the state lock and proves it by passing its handle.**
    /// This must not lock for itself: `stateMutex` is a plain `std::mutex`, so
    /// re-entering it deadlocks, and two of the three callers already hold it
    /// — `applyProfile()` over its steps 5-7 and the slot-config rebuild
    /// (both in `MainWindow_Slots.cpp`). The third, the MainWindow
    /// constructor, takes one for the call; the worker has not started there,
    /// so it is free.
    ///
    /// That contract used to live in a comment which said the opposite
    /// ("never under stateMutex"), so anyone adding the "missing" lock here
    /// would have deadlocked the profile switch on the spot. It is a
    /// parameter now: the only way to call this is to already hold the lock.
    void plugSlotsFromSettings(const pom2::StateAccess& st);

    /// Restore storage media only after the replacement cards exist. All key
    /// and mount policy lives in StorageCoordinator; this wrapper reports its
    /// non-fatal per-card diagnostics. Caller holds the state lock.
    void restoreSlotMediaFromSettings(const pom2::StateAccess& st);

    /// The body of `plugFujiNetFromCli`, on the same caller-owns-the-lock
    /// contract, so it can also run from inside `plugSlotsFromSettings()`.
    bool plugFujiNetUnlocked(const pom2::StateAccess& st,
                             int slot, bool serial,
                             const std::string& serialDevice, int tcpPort,
                             std::string& errOut);

    /// A `--fujinet` request from the command line, kept so every slot
    /// rebuild can reproduce it. Slot 0 = no request. Deliberately NOT
    /// persisted to `slot_N_card`: a one-shot CLI card must not leak into the
    /// user's saved slot configuration.
    int         cliFujiNetSlot_ = 0;
    bool        cliFujiNetSerial_ = false;
    std::string cliFujiNetSerialPath_;
    int         cliFujiNetPort_ = 1985;

    // AudioCoordinator owns the authoritative source inventory, live slot-card
    // snapshots/commands and mixer persistence. These wrappers keep source
    // registration at slot-construction call sites compact.
    void registerAudioSource(AudioSource* src);
    void unregisterAllAudioSources();

    // ── Disk insert+boot routing (shared by Disk Library UI + CLI) ───────
    // Promoted from file-local lambdas in renderDiskLibraryWindow so
    // insertAndBootImage() can reuse the exact same routing. Each takes
    // the state lock internally.
    /// Ephemeral slot inventories. Each call walks SlotBus; returned pointers
    /// are never retained as MainWindow state and are consumed within the
    /// current UI operation (under the machine lock for mutable card state).
    std::vector<DiskIICard*> diskIICards() const;
    DiskIICard* primaryDiskIICard() const;
    ProDOSHardDiskCard* primaryHdvCard() const;
    pom2::SmartPortCard* primarySmartPortCard() const;

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
    /// Flush every slot-owned medium before a profile/slot rebuild. Returns
    /// false without destroying cards so dirty RAM remains retryable.
    bool flushSlotMedia(std::string& err);
    void renderMenuBar();
    void renderStatusBar();
    void renderScreenWindow();
    /// Draw the Apple II framebuffer texture into the current ImGui
    /// window's content region, scaled + centred (letterboxed) to preserve
    /// the 4:3 aspect. Shared by the normal screen window and the kiosk
    /// full-viewport path. Updates screenRectMin/Max for Mouse Card routing.
    void drawScreenImage();
    /// Kiosk render path: a single borderless full-viewport window showing
    /// only the screen. No menu bar / toolbar / panels.
    void renderKiosk();
    void renderMemoryViewerWindow();
    // Memory map visualisations — implemented in MainWindow_MemoryMaps.cpp.
    std::vector<MemRegion> buildMemoryRegions();
    void renderMemoryBarWindow();
    void renderMemoryBarHorizontalWindow();
    void renderMemoryGridWindow();
    void renderCassetteDeckWindow(float deltaSeconds);
    void renderHgrPaintWindow();
    void renderHgrSpriteWindow();
    void renderRewindWindow(float deltaSeconds);
    // Drive hold-to-rewind from the combined input sources (F6 key + toolbar
    // button). Edge-detected against rewindHeldPrev_ — call once per frame.
    void driveRewindHold(bool held);
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
    void renderFujiNetPanelWindow();

    /// Machine ▸ "Print screen": snapshot the framebuffer and hand the
    /// printer the ESC G bit-image stream a period driver would have sent.
    /// See PrinterScreenDump.h for why it is bytes and not pixels.
    void dumpScreenToPrinter();

    /// Copy any sheets the printer has ejected since the last call into the
    /// durable history. Driven from pumpImageWriter, so it costs nothing
    /// until a page actually completes.
    void archiveNewPrinterPages();
    void renderChatMauvePanelWindow();
    void renderMockingboardPanelWindow();
    void renderPhasorPanelWindow();
    void renderEchoPlusPanelWindow();
    void renderSscPanelWindow();
    void renderEthernetPanelWindow();
    void renderPrinterPanelWindow();
    void renderImageWriterWindow();
    /// Stream new bytes from the plugged printer interface card into the
    /// host-side ImageWriter. Called once per frame from the render loop.
    void pumpImageWriter();
    void renderNoSlotClockPanelWindow();
    /// Apple //e keyboard window: lazy-loads the photo, draws it, and turns
    /// a clicked cap into a keystroke on the emulated machine.
    void renderKeyboardPanel();
    /// Lazy-load `pic/Keyboard_AppleIIe.jpeg` into `kbImageTex_`. Same
    /// once-only contract as `ensureAboutImageLoaded`.
    void ensureKeyboardImageLoaded();
    /// Abstraction Levels window: builds the live snapshot from the slot map
    /// + the card ROM-state accessors, draws it, and executes whatever
    /// boundary switch came back.
    void renderAbstractionPanel();
    /// Swap one slot card for its other-abstraction-level twin, in place.
    /// Finds the slot currently holding `fromKey`, persists `toKey` there and
    /// rebuilds the machine. False when `fromKey` is not plugged or the
    /// rebuild was refused (the live machine is left intact either way).
    bool swapSlotCardVariant(const char* fromKey, const char* toKey);
    void renderJoystickPanelWindow();
    /// Mouse Inspector — diagnostic panel for cursor-alignment tuning.
    /// Live host cursor position + Apple II Screen widget rect + Mouse
    /// Card running counter + AppleWin HLE firmware state + screen holes.
    /// Optional CSV log to `mouseInspectorLogPath` for offline review.
    void renderMouseInspectorWindow();
    void renderAudioMixerWindow();
    void renderNtscSettingsWindow();
    void renderVoxelSettingsWindow();
    void renderAiControlPanelWindow();
    void pollJoystickAndPushToMemory();
    void renderAboutDialog();
    void renderWelcomePanelWindow();
    /// Lazy-loads `pic/Apple_II_plus.jpg` into `aboutImageTex_` on the
    /// first About-dialog open. Silent no-op if the asset can't be
    /// resolved via ResourcePaths (kiosk builds, missing pic/ folder).
    void ensureAboutImageLoaded();
    /// Slot Configuration panel: per-slot card assignment (built-ins greyed).
    /// STAGED — edits land on Apply, which restarts the machine.
    /// Implemented in MainWindow_Slots.cpp.
    void renderSlotConfigPanel();
    /// Internal Disks & Media panel: the internal drives plus the mountable
    /// bays of every plugged storage card. IMMEDIATE — Mount / Insert /
    /// Eject act at once. Was the right column of Slot Configuration until
    /// 2026-07-28; one window running two interaction models is what made
    /// "mount on the right, Revert on the left" read as undoable.
    void renderMediaPanel();
    /// BMOW Floppy Emu panel — OLED-style SD browser + emulation-mode select.
    /// Routes the selected image into the matching controller card
    /// (DiskIICard / SmartPortCard units) per the device's current mode.
    void renderFloppyEmuWindow();
    /// Tear down the SlotBus and re-run plugSlotsFromSettings(). Called
    /// by the Slot Configuration panel's Apply button. Stops the
    /// emulation worker around the rebuild so card destructors / new
    /// constructors don't race against a running CPU thread.
    bool restartEmulationFromSettings();

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
    /// Eject ONE bay, addressed by slot + index (Disk II drive, or media bay
    /// on a MountableMediaCard). Re-resolves the card through the SlotBus
    /// rather than trusting a cached pointer, clears whatever settings key
    /// would otherwise remount the image next launch, and leaves the medium
    /// mounted if its write-back save failed. Drives the status-bar chips.
    bool ejectMediaBay(int slot, int index, bool diskII);

    void uploadScreenTexture();

    // Translate a GLFW key/codepoint into an ASCII byte and feed it to
    // Memory::queueKey(). Memory's strobe handling is hardware-faithful;
    // we just hand it the character.
    void injectAscii(uint8_t ascii);
};

#endif // POM2_MAIN_WINDOW_H
