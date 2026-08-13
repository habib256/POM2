// POM2 Apple II Emulator
// Cross-platform commit of a sibling temporary file over an existing file.

#ifndef POM2_ATOMIC_FILE_REPLACE_H
#define POM2_ATOMIC_FILE_REPLACE_H

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace pom2 {

inline bool replaceFileAtomic(const std::filesystem::path& from,
                              const std::filesystem::path& to,
                              std::error_code& ec)
{
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()),
                         std::system_category());
    return false;
#else
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}

} // namespace pom2

#endif // POM2_ATOMIC_FILE_REPLACE_H
