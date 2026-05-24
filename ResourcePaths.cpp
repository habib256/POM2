// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ResourcePaths.h"

#include <cstdlib>
#include <system_error>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>      // _NSGetExecutablePath
#  include <vector>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>          // GetModuleFileNameW
#else
#  include <unistd.h>           // readlink (/proc/self/exe)
#  include <limits.h>
#endif

namespace pom2 {

namespace fs = std::filesystem;

namespace {

// Best-effort absolute path of the running executable. Empty on failure.
fs::path probeExecutablePath()
{
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // first call learns the length
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(buf.data()), ec);
    return ec ? fs::path(buf.data()) : p;
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return fs::path(std::wstring(buf, n));
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self;
    // Last resort: some sandboxes don't expose /proc — give up gracefully.
    return {};
#endif
}

} // namespace

fs::path executableDir()
{
    static const fs::path cached = [] {
        fs::path exe = probeExecutablePath();
        return exe.empty() ? fs::path{} : exe.parent_path();
    }();
    return cached;
}

const std::vector<fs::path>& resourceSearchDirs()
{
    static const std::vector<fs::path> cached = [] {
        std::vector<fs::path> dirs;
        auto push = [&dirs](fs::path p) {
            std::error_code ec;
            // Keep the empty path (== CWD) verbatim; normalise the rest so
            // duplicate roots collapse (e.g. exeDir == CWD in a dev run).
            if (!p.empty()) {
                fs::path norm = fs::weakly_canonical(p, ec);
                if (!ec) p = norm;
            }
            for (const auto& d : dirs) if (d == p) return;
            dirs.push_back(std::move(p));
        };

        // 1-3: legacy CWD-relative roots (preserve exact dev behaviour).
        push(fs::path{});            // current working directory
        push("..");
        push("../..");

        // 4-6: executable-relative roots (portable bundle + FHS install).
        const fs::path exe = executableDir();
        if (!exe.empty()) {
            push(exe);                        // binary beside roms/, fonts/
            push(exe / "..");                 // binary in bin/, assets a level up
            push(exe / ".." / "share" / "POM2");  // /usr/bin + /usr/share/POM2
        }

        // 7: per-user data dir. When POM2 is installed read-only (a .deb in
        // /usr, an AppImage), the Apple ROMs the user supplies can't live
        // beside the binary — they go here, e.g. ~/.local/share/POM2/roms/.
        // Honours $XDG_DATA_HOME, else $HOME/.local/share (XDG basedir spec).
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
            push(fs::path(xdg) / "POM2");
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            push(fs::path(home) / ".local" / "share" / "POM2");
        }
        return dirs;
    }();
    return cached;
}

std::string findResource(const std::string& rel)
{
    if (rel.empty()) return {};
    std::error_code ec;

    // Absolute paths bypass the search roots entirely.
    fs::path relp(rel);
    if (relp.is_absolute()) {
        return fs::exists(relp, ec) ? rel : std::string{};
    }

    for (const fs::path& base : resourceSearchDirs()) {
        fs::path cand = base.empty() ? relp : (base / relp);
        if (fs::exists(cand, ec)) return cand.string();
    }
    return {};
}

std::string findFirstResource(const std::vector<std::string>& candidates)
{
    for (const std::string& c : candidates) {
        std::string r = findResource(c);
        if (!r.empty()) return r;
    }
    return {};
}

} // namespace pom2
