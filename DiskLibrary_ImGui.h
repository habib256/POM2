// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskLibrary_ImGui — unified browser over every disk image POM2 can
// mount: 5.25" floppies (disks/), 3.5" Sony disks (disks35/), and
// ProDOS HDV / 2IMG (hdv/). One panel, three tabs. Replaces the
// per-card "Library:" child-list section (which each card duplicated)
// with a single search-and-sort UI.
//
// Click semantics, parallel to what the per-card panels offered:
//   • 5.25" left-click  → insert into the primary DiskII card + cold boot
//     5.25" right-click → insert only (no boot, for swap-in mid-game)
//   • 3.5"  left-click  → mount drive 1 + boot via the active path
//                          (//c+ on-board OR slot-plugged SmartPortCard)
//     3.5"  right-click → context menu (drive 1/2 × mount-only/boot)
//   • HDV   left-click  → mount + boot
//     HDV   right-click → mount only
//
// Filesystem scan happens here (mtime + size sniff for size-bucketed
// dispatch), so the per-card panels can drop their own scans once this
// ships. A "Refresh" button forces an immediate re-scan; otherwise the
// scan runs once per frame (cheap — directory entries are cached by
// the OS).

#ifndef POM2_DISK_LIBRARY_IMGUI_H
#define POM2_DISK_LIBRARY_IMGUI_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace pom2 {

class DiskLibrary_ImGui
{
public:
    /// Paths the host considers currently mounted — used to flag entries
    /// with a `* ` prefix so a re-click is recognisable. Empty strings
    /// = nothing in that slot.
    struct CurrentlyMounted {
        std::vector<std::string> diskII;          // one per plugged DiskII card
        std::string              disk35Internal;
        std::string              disk35External;
        std::string              hdv;
    };

    struct Result {
        // 5.25" floppy — empty string = no action.
        std::string request525InsertAndBoot;
        std::string request525InsertOnly;
        // Path to eject (only the card(s) currently holding this path
        // are ejected). Empty = no eject.
        std::string request525EjectPath;
        // 3.5" Sony disk.
        std::string request35MountAndBoot;
        int         request35BootDrive   = 0;
        std::string request35MountOnly;
        int         request35MountDrive  = 0;
        // -1 = no eject; 0 = drive 1; 1 = drive 2.
        int         request35EjectDrive  = -1;
        // ProDOS HDV / 2IMG.
        std::string requestHdvMountAndBoot;
        std::string requestHdvMountOnly;
        bool        requestHdvEject      = false;
    };

    Result render(const char*               title,
                  bool&                     open,
                  const CurrentlyMounted&   mounted);

private:
    // ── Filesystem cache ──────────────────────────────────────────────
    struct Entry {
        std::string displayName;    // path relative to scan root
        std::string fullPath;
        uint64_t    sizeBytes  = 0;
        std::time_t mtime      = 0;
    };
    std::vector<Entry> disk525_;
    std::vector<Entry> disk35_;
    std::vector<Entry> hdv_;

    // UI state — survive between frames so search input / sort choice
    // stick.
    char searchBuf_[128]   = "";
    int  sortMode_         = 0;     // 0=name, 1=size, 2=date desc
    bool needsRescan_      = true;

    void rescan();
    void rescanInto(std::vector<Entry>& out,
                    const std::vector<const char*>& roots,
                    bool (*acceptExtAndSize)(const std::string& ext,
                                             uint64_t sz));
    void applySort(std::vector<Entry>& entries) const;
    // Returns true if `name` (case-insensitive) contains the current
    // search filter. Empty filter matches everything.
    bool passesFilter(const std::string& name) const;

    // Build the selectable list for one tab and route clicks back to
    // `r`. Caller picks the action functor so the same renderer powers
    // every tab. `markPaths` is indexed: for 5.25" / HDV one slot, for
    // 3.5" two slots (drive 1, drive 2). A path mounted in slot i sets
    // bit i in the mask passed to the context-menu callback so eject
    // items can target the right drive.
    void renderTab(const std::vector<Entry>&               entries,
                   const std::vector<std::string>&         markPaths,
                   const char*                             emptyHint,
                   void (DiskLibrary_ImGui::*onLeftClick)(const std::string&,
                                                          Result&),
                   void (DiskLibrary_ImGui::*onContextMenu)(const std::string&,
                                                            int mountedMask,
                                                            Result&),
                   Result&                                 r);

    void on525Left   (const std::string& path, Result& r);
    void on525Ctx    (const std::string& path, int mountedMask, Result& r);
    void on35Left    (const std::string& path, Result& r);
    void on35Ctx     (const std::string& path, int mountedMask, Result& r);
    void onHdvLeft   (const std::string& path, Result& r);
    void onHdvCtx    (const std::string& path, int mountedMask, Result& r);
};

} // namespace pom2

#endif // POM2_DISK_LIBRARY_IMGUI_H
