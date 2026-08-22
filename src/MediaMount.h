// POM2 Apple II Emulator
// Two-phase media mount — read the file unlocked, install it under the lock.
//
// The problem this exists to remove: `stateMutex` is taken by the CPU worker
// for every 4096-cycle chunk AND by the UI thread to paint every frame, so
// anything slow held inside it freezes the machine and the window together —
// including the button that would cancel it. Mounting a disk used to do all of
// its file I/O in there, because `DiskIICard::insertDisk(path)` reads, decodes
// and installs in one call and gave the caller no way to split it.
//
// Measured on a warm cache, so these are optimistic floors:
//     read of a 32 MB image            12.8 ms   (0.6 of a 20 ms PAL frame)
//     write of 4 MB + one fsync        30.1 ms   (1.5 frames; a commit does two)
//
// The shape here is the fix: phase 1 reads and decodes into a detached image
// with no lock held, phase 2 takes the lock only to swap the finished object
// in. Call sites become one line, which also keeps this out of MainWindow.cpp
// — the god-object the file-size ratchet is now holding still.
//
// One case deliberately keeps its cost: if the OUTGOING medium has unsaved
// changes, DiskIICard::installDisk still flushes it inline and still refuses
// the swap when that flush fails. Losing somebody's only copy of a disk to
// save a frame is not a trade worth making. Write-back is opt-in, so the
// common path is the fully unlocked one.

#ifndef POM2_MEDIA_MOUNT_H
#define POM2_MEDIA_MOUNT_H

#include <string>

class DiskIICard;
class EmulationController;

namespace pom2 {

/// Mount `path` into `card`'s `drive` (0 or 1) without holding `stateMutex`
/// across the file read.
///
/// `seekTrack0` re-seeks the head after a successful install, which is what
/// every "insert and boot" path wants; leave it false for a bare mount.
///
/// Returns false with `error` filled in — the image could not be read, or the
/// outgoing medium's write-back failed and the swap was refused.
///
/// **Caller contract**: `card` must stay alive and plugged for the whole call.
/// The two phases take `stateMutex` separately, and a profile switch nulls and
/// destroys card pointers under that same lock — so this is safe only from the
/// UI thread, which is also the thread that swaps the SlotBus (CLAUDE.md's
/// "UI-thread-confined SlotBus topology reads"). A caller on any other thread
/// must drive `prepareDisk` / `installDisk` itself and re-check its card
/// pointer under the lock before installing; `AiControlServer::handleDiskInsert`
/// is the worked example.
bool mountDiskII(EmulationController& ctrl, DiskIICard& card, int drive,
                 const std::string& path, std::string& error,
                 bool seekTrack0 = false);

} // namespace pom2

#endif // POM2_MEDIA_MOUNT_H
