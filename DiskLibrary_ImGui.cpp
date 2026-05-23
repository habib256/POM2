// POM2 Apple II Emulator
// Copyright (C) 2026

#include "DiskLibrary_ImGui.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace pom2 {

namespace {

namespace fs = std::filesystem;

// 5.25" extensions: .dsk .do .po(143360) .nib .woz .d13. We do NOT size-gate
// on the .po here — the dedicated 3.5" scanner sees the 800K bucket. `.d13`
// is the 13-sector (DOS 3.1/3.2/3.2.1) raw image (35×13×256 = 116480 B).
// `ext` arrives already lower-cased from rescanInto.
bool accept525(const std::string& ext, uint64_t sz) {
    if (ext == ".dsk" || ext == ".do" || ext == ".nib" || ext == ".woz"
        || ext == ".d13")
        return true;
    if (ext == ".po") {
        // 143 360 = 35 tracks × 16 sectors × 256 B = stock 5.25" ProDOS.
        return sz == 143360 || sz == 143360 + 64; // raw or 2IMG-wrapped
    }
    return false;
}

bool accept35(const std::string& ext, uint64_t sz) {
    if (ext != ".po" && ext != ".2mg") return false;
    // 800 K = 1600 × 512 = 819 200. 2IMG envelope adds ≤ 4 KB.
    return sz == 819200 || sz == 819200 + 64
        || (sz > 819200 && sz < 819200 + 4096);
}

bool acceptHdv(const std::string& ext, uint64_t sz) {
    if (ext != ".hdv" && ext != ".2mg") return false;
    // Anything > 800 K and a whole multiple of 512 B (or 2IMG with the
    // standard 64-byte header). Hard caps left to ProDOSHardDiskCard's
    // 32 MB ceiling.
    if (sz <= 819200) return false;
    return (sz % 512 == 0) || ((sz - 64) % 512 == 0);
}

// Case-insensitive substring scan — `needle` should already be lower-
// cased. ASCII-only, which is fine for filenames in this scope.
bool containsCi(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(),
                          needleLower.begin(), needleLower.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

std::string fmtSize(uint64_t sz) {
    char buf[32];
    if (sz < 1024u) {
        std::snprintf(buf, sizeof(buf), "%5llu B", (unsigned long long)sz);
    } else if (sz < 1024u * 1024u) {
        std::snprintf(buf, sizeof(buf), "%5.1f KB", static_cast<double>(sz) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%5.1f MB",
                      static_cast<double>(sz) / (1024.0 * 1024.0));
    }
    return buf;
}

std::string fmtDate(std::time_t t) {
    if (t == 0) return "?";
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // anon namespace

void DiskLibrary_ImGui::rescanInto(
    std::vector<Entry>&              out,
    const std::vector<const char*>&  roots,
    bool (*acceptExtAndSize)(const std::string&, uint64_t))
{
    out.clear();
    std::error_code ec;
    for (const char* dir : roots) {
        if (!fs::is_directory(dir, ec)) continue;
        const fs::path root(dir);
        for (auto it = fs::recursive_directory_iterator(root,
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const auto& de = *it;
            const std::string name = de.path().filename().string();
            // Skip dotfiles + dotdirs (.git, .DS_Store, …).
            if (!name.empty() && name.front() == '.') {
                if (de.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!de.is_regular_file(ec)) continue;
            // Lower-case the extension so MAJUSCULE dumps (DOS32PLS.D13,
            // DOS13SEC.DSK) match the accept predicates' lower-case literals.
            std::string ext = de.path().extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto sz = static_cast<uint64_t>(de.file_size(ec));
            if (ec) continue;
            if (!acceptExtAndSize(ext, sz)) continue;

            Entry e;
            e.displayName = fs::relative(de.path(), root, ec).string();
            if (e.displayName.empty()) e.displayName = name;
            e.fullPath    = de.path().string();
            e.sizeBytes   = sz;
            // mtime → time_t via filesystem's clock cast.
            const auto ftime = de.last_write_time(ec);
            if (!ec) {
                const auto sctp =
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - decltype(ftime)::clock::now()
                              + std::chrono::system_clock::now());
                e.mtime = std::chrono::system_clock::to_time_t(sctp);
            }
            out.push_back(std::move(e));
        }
        if (!out.empty()) break;     // first existing root wins
    }
}

void DiskLibrary_ImGui::rescan()
{
    rescanInto(disk525_,
               { "disks", "../disks", "../../disks" },
               &accept525);
    rescanInto(disk35_,
               { "disks35", "../disks35", "../../disks35",
                 "disks",   "../disks",   "../../disks" },
               &accept35);
    rescanInto(hdv_,
               { "hdv", "../hdv", "../../hdv" },
               &acceptHdv);
    needsRescan_ = false;
}

void DiskLibrary_ImGui::applySort(std::vector<Entry>& entries) const
{
    switch (sortMode_) {
        case 1:     // size ↓
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) {
                    return a.sizeBytes > b.sizeBytes;
                });
            break;
        case 2:     // date ↓
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) {
                    return a.mtime > b.mtime;
                });
            break;
        case 0:
        default:    // name ↑
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) {
                    return a.displayName < b.displayName;
                });
            break;
    }
}

bool DiskLibrary_ImGui::passesFilter(const std::string& name) const
{
    if (searchBuf_[0] == '\0') return true;
    std::string needle(searchBuf_);
    for (char& c : needle) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    return containsCi(name, needle);
}

void DiskLibrary_ImGui::on525Left(const std::string& path, Result& r)
{
    r.request525InsertAndBoot = path;
}
void DiskLibrary_ImGui::on525Ctx(const std::string& path, int mountedMask, Result& r)
{
    (void)mountedMask;
    const CurrentlyMounted* m = mounted_;

    // Fallback when the host gave no per-card info: legacy single-target.
    if (!m || m->diskIICards.empty()) {
        if (ImGui::MenuItem("Insert + boot (slot 6 / primary)")) {
            r.request525InsertAndBoot = path;
            r.request525Slot = -1; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Insert only (no boot — hot-swap)")) {
            r.request525InsertOnly = path;
            r.request525Slot = -1; r.request525Drive = 0;
        }
        return;
    }

    // Emit the three mount targets (+ eject) for one DiskII card. drive 1 is
    // bootable; drive 2 is data-only (the boot PROM boots drive 1).
    auto emitCard = [&](const CurrentlyMounted::DiskIICardInfo& card) {
        if (ImGui::MenuItem("Drive 1: insert + boot")) {
            r.request525InsertAndBoot = path;
            r.request525Slot = card.slot; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Drive 1: insert only")) {
            r.request525InsertOnly = path;
            r.request525Slot = card.slot; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Drive 2: insert only")) {
            r.request525InsertOnly = path;
            r.request525Slot = card.slot; r.request525Drive = 1;
        }
        if (card.drive1 == path || card.drive2 == path) {
            ImGui::Separator();
            if (ImGui::MenuItem("Eject this image")) r.request525EjectPath = path;
        }
    };

    // One card → flat items; several DiskII cards → one submenu per slot.
    if (m->diskIICards.size() == 1) {
        emitCard(m->diskIICards.front());
    } else {
        for (const auto& card : m->diskIICards) {
            char hdr[24];
            std::snprintf(hdr, sizeof(hdr), "Slot %d", card.slot);
            if (ImGui::BeginMenu(hdr)) {
                emitCard(card);
                ImGui::EndMenu();
            }
        }
    }
}
void DiskLibrary_ImGui::on35Left(const std::string& path, Result& r)
{
    r.request35MountAndBoot = path;
    r.request35BootDrive    = 0;
}
void DiskLibrary_ImGui::on35Ctx(const std::string& path, int mountedMask, Result& r)
{
    if (ImGui::MenuItem("Mount on drive 1 + boot")) {
        r.request35MountAndBoot = path;
        r.request35BootDrive    = 0;
    }
    if (ImGui::MenuItem("Mount on drive 1 (no boot)")) {
        r.request35MountOnly    = path;
        r.request35MountDrive   = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Mount on drive 2 + boot")) {
        r.request35MountAndBoot = path;
        r.request35BootDrive    = 1;
    }
    if (ImGui::MenuItem("Mount on drive 2 (no boot)")) {
        r.request35MountOnly    = path;
        r.request35MountDrive   = 1;
    }
    if (mountedMask & 0x3) {
        ImGui::Separator();
        if ((mountedMask & 0x1) && ImGui::MenuItem("Eject from drive 1")) {
            r.request35EjectDrive = 0;
        }
        if ((mountedMask & 0x2) && ImGui::MenuItem("Eject from drive 2")) {
            r.request35EjectDrive = 1;
        }
    }
}
void DiskLibrary_ImGui::onHdvLeft(const std::string& path, Result& r)
{
    r.requestHdvMountAndBoot = path;
}
void DiskLibrary_ImGui::onHdvCtx(const std::string& path, int mountedMask, Result& r)
{
    if (ImGui::MenuItem("Mount + boot")) {
        r.requestHdvMountAndBoot = path;
    }
    if (ImGui::MenuItem("Mount only (no boot)")) {
        r.requestHdvMountOnly = path;
    }
    if (mountedMask & 0x1) {
        ImGui::Separator();
        if (ImGui::MenuItem("Eject")) {
            r.requestHdvEject = true;
        }
    }
}

void DiskLibrary_ImGui::renderTab(
    const std::vector<Entry>&        entries,
    const std::vector<std::string>&  markPaths,
    const char*                      emptyHint,
    void (DiskLibrary_ImGui::*onLeftClick)(const std::string&, Result&),
    void (DiskLibrary_ImGui::*onContextMenu)(const std::string&, int, Result&),
    Result&                          r)
{
    // Filter + sort happen on a local copy so toggling sort doesn't
    // mutate the cached scan.
    std::vector<Entry> filtered;
    filtered.reserve(entries.size());
    for (const auto& e : entries) {
        if (passesFilter(e.displayName)) filtered.push_back(e);
    }
    applySort(filtered);

    if (filtered.empty()) {
        ImGui::TextDisabled("%s", emptyHint);
        return;
    }

    // Slightly taller child than the per-card variants so the toolbar
    // pinning leaves room and the table stays comfortable.
    ImGui::BeginChild("##library_table", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    if (ImGui::BeginTable("##library_grid", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,  64.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed,  80.0f);
        ImGui::TableHeadersRow();

        for (const auto& e : filtered) {
            ImGui::TableNextRow();
            int mountedMask = 0;
            for (size_t i = 0; i < markPaths.size() && i < 8; ++i) {
                if (!markPaths[i].empty() && markPaths[i] == e.fullPath)
                    mountedMask |= (1 << i);
            }
            const bool mounted = mountedMask != 0;
            ImGui::PushID(e.fullPath.c_str());

            ImGui::TableSetColumnIndex(0);
            const std::string label = (mounted ? "* " : "  ") + e.displayName;
            if (ImGui::Selectable(label.c_str(), mounted,
                                  ImGuiSelectableFlags_SpanAllColumns))
            {
                (this->*onLeftClick)(e.fullPath, r);
            }
            if (ImGui::BeginPopupContextItem("ctx")) {
                (this->*onContextMenu)(e.fullPath, mountedMask, r);
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(fmtSize(e.sizeBytes).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fmtDate(e.mtime).c_str());

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

DiskLibrary_ImGui::Result DiskLibrary_ImGui::render(
    const char*               title,
    bool&                     open,
    const CurrentlyMounted&   mounted)
{
    Result r;
    if (!open) return r;
    mounted_ = &mounted;     // for on525Ctx's per-drive target enumeration

    // No SetNextWindowSize here — the host pre-applies a curated default
    // via SetNextWindowPos/Size (see MainWindow::renderDiskLibraryWindow).
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    if (needsRescan_) rescan();

    // ── Header row: refresh + search + sort ───────────────────────────
    if (ImGui::Button(ICON_FA_ROTATE " Refresh")) needsRescan_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-scan disks/, disks35/, hdv/");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##library_search", "search...",
                             searchBuf_, sizeof(searchBuf_));

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted("Sort:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    static const char* kSortLabels[] = { "Name", "Size", "Date" };
    ImGui::Combo("##library_sort", &sortMode_, kSortLabels,
                 IM_ARRAYSIZE(kSortLabels));

    // Eject-all lives at the top of the Library (moved here from the
    // toolbar) so the one window that mounts disks also unmounts them.
    // Disabled unless something is actually mounted on any path.
    const bool anyMounted =
        !mounted.diskII.empty()        || !mounted.disk35Internal.empty() ||
        !mounted.disk35External.empty() || !mounted.hdv.empty();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!anyMounted);
    if (ImGui::Button(ICON_FA_EJECT " Eject All"))
        r.requestEjectAllDisks = true;
    ImGui::EndDisabled();
    if (anyMounted && ImGui::IsItemHovered())
        ImGui::SetTooltip("Eject every loaded Disk II / HDV / SmartPort image");

    ImGui::Separator();
    ImGui::TextDisabled(
        "left-click = insert + boot      right-click = more options");

    // ── Tabs ───────────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("##library_tabs", ImGuiTabBarFlags_Reorderable)) {
        char tabLabel[64];
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_FLOPPY_DISK " 5.25\"  (%zu)", disk525_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            renderTab(disk525_, mounted.diskII,
                      "  (drop .dsk / .do / .po / .nib / .woz / .d13 into disks/)",
                      &DiskLibrary_ImGui::on525Left,
                      &DiskLibrary_ImGui::on525Ctx,
                      r);
            ImGui::EndTabItem();
        }
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_FLOPPY_DISK " 3.5\"  (%zu)", disk35_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            std::vector<std::string> marks35 = {
                mounted.disk35Internal, mounted.disk35External };
            renderTab(disk35_, marks35,
                      "  (drop 800K .po / .2mg into disks35/)",
                      &DiskLibrary_ImGui::on35Left,
                      &DiskLibrary_ImGui::on35Ctx,
                      r);
            ImGui::EndTabItem();
        }
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_HARD_DRIVE  " HDV   (%zu)", hdv_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            std::vector<std::string> marksHdv = { mounted.hdv };
            renderTab(hdv_, marksHdv,
                      "  (drop .hdv / .2mg into hdv/)",
                      &DiskLibrary_ImGui::onHdvLeft,
                      &DiskLibrary_ImGui::onHdvCtx,
                      r);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    return r;
}

} // namespace pom2
