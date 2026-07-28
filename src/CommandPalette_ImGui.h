// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CommandPalette_ImGui — fuzzy "type what you want" launcher (Ctrl+Shift+P).
//
// POM2 has 42 menu items across 8 menus plus ~33 toggleable panels, and only
// four keyboard shortcuts (F6/F9/F11/F12). Finding "Mockingboard" means
// remembering it lives under Devices ▸ Sound; finding the char-ROM locale
// means remembering it is a toolbar combo and not a menu at all. The palette
// removes the remembering: type "mock", "amber", "pal", "eject" and hit Enter.
//
// Same snapshot/result contract as the other `*_ImGui` panels: the host
// (`MainWindow`) fills a command list when the palette opens, the palette does
// the matching and the keyboard work, and returns the id of whatever the user
// picked. The palette never touches the emulator — it doesn't even know what a
// command *does*, only its label. That keeps dispatch in one place
// (`MainWindow::runCommand`) and makes a command's availability the host's
// business.

#ifndef POM2_COMMANDPALETTE_IMGUI_H
#define POM2_COMMANDPALETTE_IMGUI_H

#include <string>
#include <vector>

namespace pom2 {

class CommandPalette_ImGui
{
public:
    struct Command {
        std::string id;        ///< Stable dispatch key, never shown.
        std::string label;     ///< What the user reads and searches.
        std::string category;  ///< Grouping hint, shown dimmed ("Devices").
        std::string shortcut;  ///< Key hint, right-aligned ("F6"), may be empty.
        /// Greyed and unselectable. Kept in the list rather than filtered out:
        /// seeing "Phasor (no card plugged)" teaches where the thing lives,
        /// which silently omitting it does not.
        bool enabled = true;
        /// Toggle state, drawn as a check. Purely informational.
        bool checked = false;
    };

    struct Result {
        bool        executed = false;
        std::string commandId;
    };

    /// Show the palette and reset the query/selection. Idempotent while open.
    void open();
    void close();
    bool isOpen() const { return open_; }

    /// Replace the command list. The host calls this every frame the palette
    /// is open so `enabled` / `checked` track live machine state.
    void setCommands(std::vector<Command> cmds) { commands_ = std::move(cmds); }

    /// Draw and handle input. Returns the picked command, if any.
    Result render();

private:
    bool                 open_        = false;
    bool                 focusQueued_ = false;
    int                  selected_    = 0;
    char                 query_[128]  = {};
    std::vector<Command> commands_;
    /// Indices into `commands_` that matched, best first. Rebuilt each frame;
    /// a member rather than a local so the buffer is reused.
    std::vector<int>     matches_;
};

/// Case-insensitive fuzzy subsequence score. Returns a value >= 0 when every
/// character of `needle` appears in `hay` in order, or -1 when it does not.
/// An empty needle matches everything with score 0. Exposed for testing.
int fuzzyScore(const char* needle, const char* hay);

} // namespace pom2

#endif // POM2_COMMANDPALETTE_IMGUI_H
