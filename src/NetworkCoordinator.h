// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Host-network coordinator. Device models retain their emulated registers;
// this class owns host transport selection, FujiNet helper lifecycle and the
// cached host-side discovery/status state shared by construction and UI.

#ifndef POM2_NETWORK_COORDINATOR_H
#define POM2_NETWORK_COORDINATOR_H

#include "FujiNet_ImGui.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class EmulationController;

namespace pom2 {

class FujiNetCard;
class FujiNetHost;
class NetworkBackend;
class Settings;
class SpOverSlipLink;
class W5100SocketFactory;

class NetworkCoordinator
{
public:
    NetworkCoordinator();
    ~NetworkCoordinator();

    NetworkCoordinator(const NetworkCoordinator&) = delete;
    NetworkCoordinator& operator=(const NetworkCoordinator&) = delete;

    std::unique_ptr<NetworkBackend> makeEthernetBackend(
        const Settings& settings, const char* consumer) const;
    std::unique_ptr<W5100SocketFactory> makeW5100SocketFactory() const;

    /// Restore and optionally start one FujiNet link, plus the helper path
    /// belonging to that slot. A failed start is non-fatal and becomes status.
    void restoreFujiNet(SpOverSlipLink& link, const Settings& settings, int slot);
    void persistFujiNet(Settings& settings, const FujiNetCard& card,
                        const SpOverSlipLink& link) const;
    void persistFujiNet(Settings& settings,
                        EmulationController& controller) const;

    /// Immutable panel boundary. The card and concrete runtime adapter are
    /// resolved from SlotBus while the state lock is held and never escape.
    FujiNet_ImGui::Snapshot captureFujiNetPanel(
        EmulationController& controller);
    void applyFujiNetPanel(EmulationController& controller,
                           const FujiNet_ImGui::Result& command);

    void rescanSerialDevices();
    const std::vector<std::pair<std::string, std::string>>& serialDevices() const
    { return serialDevices_; }

    const std::string& status() const noexcept { return status_; }
    void setStatus(std::string status) { status_ = std::move(status); }
    void clearStatus() { status_.clear(); }

    const std::string& helperPath() const noexcept { return helperPath_; }
    const std::string& helperResolved() const noexcept { return helperResolved_; }
    void setHelperPath(std::string path);
    bool startHelper();
    void stopHelper();
    bool helperRunning();
    int helperExitCode() const;

    /// Stop runtime resources before the slot bus destroys its card/link.
    void shutdown(SpOverSlipLink* link);
    void shutdownFujiNet(EmulationController& controller);

private:
    std::unique_ptr<FujiNetHost> host_;
    std::string status_;
    std::vector<std::pair<std::string, std::string>> serialDevices_;
    std::string helperPath_;
    std::string helperResolved_;
};

} // namespace pom2

#endif // POM2_NETWORK_COORDINATOR_H
