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
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace pom2 {

/// Push a file's CONTENTS to stable storage.
///
/// `rename` is atomic with respect to a READER — it never exposes a half
/// written file — but it says nothing about DURABILITY. Until the data pages
/// are flushed, a power cut (or a kernel panic, or a yanked USB stick) can
/// land the directory entry while the blocks behind it are still in page
/// cache: the classic outcome is a 0-byte file where the user's disk image
/// used to be, because the metadata commit reached the journal and the data
/// did not. Every write-back path in POM2 replaces the user's ONLY copy of
/// their media — the rest of the disk lives in RAM and dies with the process
/// — so this flush is not optional here.
///
/// Returns false only on a REAL I/O failure (EIO, ENOSPC): the caller should
/// then treat the save as failed and keep its dirty state so the user can
/// retry. A filesystem that simply cannot honour the request (EINVAL /
/// EOPNOTSUPP — some network and virtual filesystems, Emscripten's MEMFS)
/// reports success: failing the user's save over a missing guarantee would
/// be strictly worse than saving without it.
inline bool syncFileContents(const std::filesystem::path& p,
                             std::error_code& ec)
{
#ifdef _WIN32
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()),
                             std::system_category());
        return false;
    }
    const BOOL  ok  = FlushFileBuffers(h);
    const DWORD err = ok ? 0u : GetLastError();
    CloseHandle(h);
    if (ok) { ec.clear(); return true; }
    // The volume does not implement write-through (ERROR_INVALID_FUNCTION is
    // what FlushFileBuffers returns on several network redirectors).
    if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(err), std::system_category());
    return false;
#else
    // O_RDONLY is enough for fsync on Linux/macOS/BSD, and it is what lets
    // this run AFTER the caller has restored the original file's permissions
    // onto the temp copy (which may be read-only).
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = (rc == 0) ? 0 : errno;
    ::close(fd);
    if (rc == 0) { ec.clear(); return true; }
    if (saved == EINVAL || saved == ENOTSUP || saved == EOPNOTSUPP ||
        saved == EBADF  || saved == EROFS) {
        ec.clear();               // cannot promise it → not a write failure
        return true;
    }
    ec = std::error_code(saved, std::generic_category());
    return false;
#endif
}

/// Flush the DIRECTORY ENTRY a freshly renamed file lives in, so the rename
/// itself survives a crash (POSIX requires the containing directory to be
/// fsync'd for that; the data flush above only covers the file's blocks).
///
/// Best-effort by design: a fair number of filesystems refuse fsync on a
/// directory handle, and none of those refusals make the preceding data
/// flush any less valid. On Windows the rename already goes out with
/// MOVEFILE_WRITE_THROUGH, which is the documented equivalent.
inline void syncParentDirectory(const std::filesystem::path& p) noexcept
{
#ifdef _WIN32
    (void)p;
#else
    std::error_code ignored;
    const std::filesystem::path dir =
        p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
#  ifdef O_DIRECTORY
    const int flags = O_RDONLY | O_DIRECTORY;
#  else
    const int flags = O_RDONLY;
#  endif
    const int fd = ::open(dir.c_str(), flags);
    if (fd < 0) return;
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    (void)rc;
    ::close(fd);
#endif
}

inline bool replaceFileAtomic(const std::filesystem::path& from,
                              const std::filesystem::path& to,
                              std::error_code& ec)
{
    // Data first: the temp file's contents must be on the medium BEFORE the
    // rename publishes them, or the swap can expose blocks still in flight.
    if (!syncFileContents(from, ec)) return false;
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
    if (ec) return false;
    syncParentDirectory(to);   // ...then the directory entry that names them
    return true;
#endif
}

} // namespace pom2

#endif // POM2_ATOMIC_FILE_REPLACE_H
