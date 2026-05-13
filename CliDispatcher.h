// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CliDispatcher — parses POM2's command line into a CliPlan. Inspired by
// POM1's 3-phase parser, narrowed to Apple II / II+ verbs.
//
//   Phase A (boot)       : preset, --speed/--cpu-max, initial tape path,
//                          save-tape path, snapshot-load (start state).
//   Phase B (first frame): apply the speed override + tape path inside
//                          MainWindow's first render() call.
//   Phase C (deferred)   : --load addr:file, --run addr, --paste, --step,
//                          --trace-brk, --play/--rec/--rewind, --snapshot-
//                          save. Fires after a short settling period so a
//                          snapshot-load (Phase A) is fully applied first.
//
// Every parser is dependency-free for unit testing — pass argv,
// receive std::optional<CliPlan>.

#ifndef POM2_CLI_DISPATCHER_H
#define POM2_CLI_DISPATCHER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

/// Save-tape format hint. NoHint defers to the path's file extension.
enum class CliSaveTapeFormat { NoHint, Aci, Wav };

/// Initial hi-res rendering mode. NoHint leaves the Apple2Display default
/// (Color NTSC) untouched. The other values mirror Apple2Display::HiResMode
/// without making this header depend on the display class.
enum class CliDisplayMode {
    NoHint,
    ColorNTSC,
    ChatMauveRGB,
    MonoWhite,
    MonoGreen,
    MonoAmber,
};

/// Apple II preset. Maps 1:1 to `pom2::SystemProfile` plus a `Default`
/// sentinel meaning "use the persisted/profile-default selection from
/// Settings". When non-Default, the CliDispatcher commands the MainWindow
/// to apply that profile via `MainWindow::applyProfile(...)` during
/// Phase A boot.
enum class CliPreset {
    Default,
    AppleII,        // → pom2::SystemProfile::AppleII
    AppleIIPlus,    // → pom2::SystemProfile::AppleIIPlus
    AppleIIe,       // → pom2::SystemProfile::AppleIIe
    AppleIIc,       // → pom2::SystemProfile::AppleIIc
};

/// One deferred action consumed in Phase C, in CLI order.
struct CliAction {
    enum class Kind {
        Load,          // addressI + pathS  : memWrite the file at addressI
        Run,           // addressI          : setProgramCounter + start running
        Paste,         // pathS             : feed file content as keystrokes (≤ 4096)
        Step,          // countI            : single-step countI times
        TraceBrk,      // (no args)         : enable BRK trace dump
        PlayTape,      // (no args)         : cassette PLAY
        RecTape,       // (no args)         : cassette REC arm
        RewindTape,    // (no args)         : cassette REW
        SnapshotSave,  // pathS             : write current state to .snap
        SnapshotLoad,  // pathS             : restore from .snap
    };

    Kind        kind;
    int         addressI = 0;
    int         countI   = 0;
    std::string pathS;
};

struct CliPlan {
    // Phase A — read by main() / MainWindow constructor.
    CliPreset                       preset = CliPreset::Default;
    bool                            cpuMax = false;
    /// `--ii-plus`: ignore `roms/apple2e.rom` even when present and force
    /// II+ mode. Useful for software that boots cleanly under the legacy
    /// 12 KB ROM but trips a still-unresolved IIe-paging bug under the
    /// 16 KB Enhanced ROM.
    bool                            forceIIPlus = false;
    std::optional<int>              executionSpeed;        // cycles/frame
    std::string                     initialTapePath;       // --tape <path>
    bool                            initialTapeAutoPlay = false;
    std::string                     saveTapePath;          // --save-tape <path>
    CliSaveTapeFormat               saveTapeFormat = CliSaveTapeFormat::NoHint;
    CliDisplayMode                  displayMode = CliDisplayMode::NoHint;

    // Phase C.
    std::vector<CliAction>          deferredActions;
};

/// Parse argv. Returns:
///   * `std::nullopt` and prints to stderr on parse error (caller exits ≠0).
///   * Populated CliPlan otherwise.
///
/// `helpRequestedOut` is set to true if `--help` / `-h` was seen — the
/// usage was printed and the caller should exit 0 without continuing.
std::optional<CliPlan> parseCli(int argc, char* argv[], bool& helpRequestedOut);

/// Run every Phase-C action in `plan.deferredActions`, in order. Errors
/// are logged; the first fatal error short-circuits the rest.
void runDeferredActions(const std::vector<CliAction>& actions,
                        EmulationController& emu);

/// Build a save-tape path honouring `--save-tape-format` when the path
/// has no recognisable extension. Used by main() before handing the path
/// to the cassette device.
std::string resolveSaveTapePath(const std::string& path, CliSaveTapeFormat hint);

} // namespace pom2

#endif // POM2_CLI_DISPATCHER_H
