// POM2 Apple II Emulator
// Two-phase media mount — see MediaMount.h for why this exists.

#include "MediaMount.h"

#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"

#include <mutex>
#include <utility>

namespace pom2 {

bool mountDiskII(EmulationController& ctrl, DiskIICard& card, int drive,
                 const std::string& path, std::string& error,
                 bool seekTrack0)
{
    error.clear();

    // Phase 1 — no lock. The read and the nibble decode happen here, so the
    // CPU worker keeps running and the UI keeps painting through all of it.
    DiskImage prepared;
    if (!DiskIICard::prepareDisk(path, card.isWriteBackEnabled(), prepared, error))
        return false;

    // Phase 2 — the lock, held only for the swap. Same mutex the CPU worker
    // takes around softSwitchAccess: installing rebuilds the drive's track
    // buffers, so it must not race the LSS.
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ok = card.installDisk(drive, std::move(prepared));
        if (ok) {
            if (seekTrack0) card.seekTrack0();
        } else {
            error = card.getLastError(drive);
        }
    }
    if (!ok && error.empty()) error = "insert failed";
    return ok;
}

} // namespace pom2
