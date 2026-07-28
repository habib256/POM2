// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskLibrary_ImGui — unified browser over every disk image POM2 can
// mount: 5.25" floppies (disks_5.4/), 3.5" Sony disks (disks_3.5/), and
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
#include <map>
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
        // Every mounted 5.25" path (any card, any drive) — for "* " flagging.
        std::vector<std::string> diskII;
        // Per plugged DiskII card: its slot + each drive's mounted path
        // (empty = empty drive). Lets the 5.25" right-click menu offer every
        // present drive as a target, not just the primary card's drive 1.
        struct DiskIICardInfo {
            int         slot = 6;
            std::string drive1;   // empty = no disk in drive 1
            std::string drive2;   // empty = no disk in drive 2
        };
        std::vector<DiskIICardInfo> diskIICards;
        std::string              disk35Internal;
        std::string              disk35External;
        std::string              hdv;
    };

    struct Result {
        // 5.25" floppy — empty string = no action.
        std::string request525InsertAndBoot;
        std::string request525InsertOnly;
        // Target for the insert above: DiskII card slot (-1 = primary/lowest)
        // and drive (0 = drive 1, 1 = drive 2). Set by the right-click menu so
        // an image can be mounted into ANY present 5.25" drive.
        int         request525Slot  = -1;
        int         request525Drive = 0;
        // Path to eject (only the card(s)/drive(s) currently holding this path
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
        // Eject every loaded image at once (header-row "Eject All" button).
        bool        requestEjectAllDisks = false;
        // Path whose favourite state the user flipped (right-click menu).
        // Empty = no change. The host owns the set and persists it.
        std::string toggleFavourite;
        // User clicked the Size/Date column visibility checkbox.
        bool        toggleHideSizeDate = false;
    };

    /// Host-owned lists the panel renders but does not own.
    ///
    /// Favourites and recents live in `MainWindow` (persisted to `state.cfg`
    /// as `library_favourites` / `library_recents`) rather than here, for the
    /// same reason the mounted paths do: this panel has no Settings access and
    /// no business acquiring one. It reports a toggle through `Result` and the
    /// host decides what to keep.
    struct Lists {
        std::vector<std::string> favourites;   ///< Any tab, matched by full path.
        std::vector<std::string> recents;      ///< Most-recent first.
        /// Drop the Size / Date columns. Host-owned so the choice survives a
        /// restart (persisted as `library_hide_sizedate`) — a display
        /// preference that resets every launch is one the user re-sets every
        /// launch. Matters most in a narrow dock, where the two fixed columns
        /// eat the room the name needs.
        bool hideSizeDate = false;
    };

    Result render(const char*               title,
                  bool&                     open,
                  const CurrentlyMounted&   mounted,
                  const Lists&              lists);

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
    bool needsRescan_      = true;
    // Stashed for the duration of render() so the 5.25" context-menu callback
    // can enumerate every plugged DiskII card/drive as a mount target.
    const CurrentlyMounted* mounted_ = nullptr;
    // Ditto for the host's favourite / recent lists, so the shared row
    // renderer and the context menus can consult them without threading an
    // extra parameter through every callback signature.
    const Lists*            lists_   = nullptr;

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
    /// True when `path` is in the host's favourites list.
    bool isFavourite(const std::string& path) const;

    // ── Folder tree ───────────────────────────────────────────────────
    // Built per frame from the filtered entry list. The first cut rendered
    // folders as FLAT labels carrying their full prefix ("demo/French Touch
    // Demos" as a sibling of "demo"), which is not a tree — it reads as a
    // list of unrelated folders and gives no indentation to follow. This is
    // a real nested structure: `demo` > `French Touch Demos`.
    //
    // `kids` is a std::map so siblings come out name-ordered without a
    // separate sort, and folders render before files at each level.
    struct TreeNode {
        std::vector<const Entry*>      files;   // files directly in this dir
        std::map<std::string, TreeNode> kids;   // subdirectories, name-ordered
    };

    /// Everything renderRow needs, bundled so the recursive tree walk doesn't
    /// carry six parameters down every level.
    struct RowContext {
        const std::vector<std::string>* markPaths;
        void (DiskLibrary_ImGui::*onLeftClick)(const std::string&, Result&);
        void (DiskLibrary_ImGui::*onContextMenu)(const std::string&, int, Result&);
        bool showSizeDate;
    };

    void renderTreeNode(const TreeNode& node, const RowContext& ctx, Result& r);

    /// Draw one table row (full-row selectable + favourite star + name + size
    /// + date) and route clicks. Shared by the flat list, the folder tree and
    /// the Favourites / Recent sections so all four look and behave alike.
    /// `nameOverride` is used by the tree to show a basename instead of the
    /// full relative path.
    void renderRow(const Entry&      e,
                   const char*       nameOverride,
                   const RowContext& ctx,
                   Result&           r);

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
