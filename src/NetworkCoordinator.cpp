// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "NetworkCoordinator.h"

#include "ClockCard.h"
#include "EmulationController.h"
#include "FujiNetCard.h"
#include "FujiNetHost.h"
#include "Logger.h"
#include "GrapplerCard.h"
#include "NetworkBackend.h"
#include "SerialPort.h"
#include "Settings.h"
#include "SlirpNetworkBackend.h"
#include "SpOverSlipLink.h"
#include "SpTransport.h"
#include "SlotBus.h"
#include "PrinterCard.h"
#include "W5100HostSockets.h"

namespace pom2 {
namespace {

FujiNetCard* findFujiNet(SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<FujiNetCard*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

template <typename Card>
bool hasCard(const SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (dynamic_cast<const Card*>(bus.peripheral(slot))) return true;
    }
    return false;
}

} // namespace

NetworkCoordinator::NetworkCoordinator()
    : host_(std::make_unique<FujiNetHost>())
{
}

NetworkCoordinator::~NetworkCoordinator()
{
    shutdown(nullptr);
}

std::unique_ptr<NetworkBackend> NetworkCoordinator::makeEthernetBackend(
    const Settings& settings, const char* consumer) const
{
    const std::string choice = settings.getString("ethernet_backend", "slirp");
    if (choice == "loopback")
        return std::make_unique<LoopbackNetworkBackend>();
    if (choice == "none")
        return std::make_unique<NullNetworkBackend>();

    if (!slirpAvailable()) {
        log().warn(consumer,
            "libslirp not compiled in — no host Ethernet transport. "
            "Uthernet II TCP/UDP still works; install libslirp-dev and "
            "rebuild for raw-frame modes and the Uthernet I.");
        return std::make_unique<NullNetworkBackend>();
    }
    auto backend = makeSlirpBackend("pom2");
    if (!backend) {
        log().warn(consumer,
            "libslirp failed to start — falling back to no host transport");
        return std::make_unique<NullNetworkBackend>();
    }
    return backend;
}

std::unique_ptr<W5100SocketFactory>
NetworkCoordinator::makeW5100SocketFactory() const
{
    return makeW5100HostSocketFactory();
}

void NetworkCoordinator::restoreFujiNet(SpOverSlipLink& link,
                                        const Settings& settings, int slot)
{
    const std::string suffix = "_slot" + std::to_string(slot);
    link.setTimeoutMs(settings.getInt("fujinet_timeout_ms" + suffix,
                                     SpOverSlipLink::kDefaultTimeoutMs));

    if (settings.getString("fujinet_transport" + suffix, "tcp") == "serial") {
        link.setSerialMode(
            settings.getString("fujinet_serial_path" + suffix, ""),
            settings.getInt("fujinet_serial_baud" + suffix,
                            SerialPort::kDefaultBaud));
    } else {
        link.setTcpMode(static_cast<uint16_t>(
            settings.getInt("fujinet_port" + suffix,
                            SpTcpTransport::kDefaultPort)));
    }

    if (settings.getBool("fujinet_enabled" + suffix, true)) {
        std::string error;
        if (!link.start(error)) {
            status_ = error;
            log().warn("FujiNet", "link not started: " + error);
        } else {
            status_.clear();
        }
    }

    setHelperPath(settings.getString("fujinet_helper_path" + suffix, ""));
}

void NetworkCoordinator::persistFujiNet(Settings& settings,
                                        const FujiNetCard& card,
                                        const SpOverSlipLink& link) const
{
    const std::string suffix = "_slot" + std::to_string(card.getSlot());
    settings.setBool("fujinet_enabled" + suffix, link.isRunning());
    settings.setInt("fujinet_timeout_ms" + suffix, link.timeoutMs());
    settings.setString("fujinet_transport" + suffix,
        link.mode() == SpOverSlipLink::Mode::Serial ? "serial" : "tcp");
    settings.setInt("fujinet_port" + suffix, link.tcpPort());
    settings.setString("fujinet_serial_path" + suffix, link.serialPath());
    settings.setInt("fujinet_serial_baud" + suffix, link.serialBaud());
    settings.setString("fujinet_helper_path" + suffix, helperPath_);
}

void NetworkCoordinator::persistFujiNet(
    Settings& settings, EmulationController& controller) const
{
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    auto* card = findFujiNet(bus);
    auto* link = card
        ? dynamic_cast<SpOverSlipLink*>(card->link()) : nullptr;
    if (card && link) persistFujiNet(settings, *card, *link);
}

FujiNet_ImGui::Snapshot NetworkCoordinator::captureFujiNetPanel(
    EmulationController& controller)
{
    FujiNet_ImGui::Snapshot snapshot;

    // Host-only state does not need the emulation lock and some of it may
    // consult a child process. Capture it before entering the critical path.
    snapshot.serialDevices = serialDevices_;
    snapshot.helperRunning = helperRunning();
    snapshot.helperPath = helperPath_;
    snapshot.helperExitCode = helperExitCode();
    snapshot.helperResolved = helperResolved_;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = findFujiNet(bus);
        if (!card) return snapshot;
        auto* link = dynamic_cast<SpOverSlipLink*>(card->link());
        if (!link) return snapshot;

        snapshot.plugged = true;
        snapshot.slot = card->getSlot();
        snapshot.transport =
            link->mode() == SpOverSlipLink::Mode::Serial
                ? FujiNet_ImGui::Transport::Serial
            : link->mode() == SpOverSlipLink::Mode::Tcp
                ? FujiNet_ImGui::Transport::Tcp
                : FujiNet_ImGui::Transport::Off;
        snapshot.running = link->isRunning();
        snapshot.connected = link->isConnected();
        snapshot.state = link->describe();
        snapshot.lastError = link->lastError();
        snapshot.tcpPort = link->tcpPort();
        snapshot.serialPath = link->serialPath();
        snapshot.serialBaud = link->serialBaud();
        snapshot.timeoutMs = link->timeoutMs();
        for (const auto& device : link->devices()) {
            FujiNet_ImGui::DeviceRow row;
            row.unit = device.unit;
            row.name = device.name;
            row.type = device.type;
            row.subtype = device.subtype;
            row.blocks = device.blocks;
            snapshot.devices.push_back(std::move(row));
        }
        const auto stats = link->stats();
        snapshot.calls = stats.calls;
        snapshot.timeouts = stats.timeouts;
        snapshot.stale = stats.stale;
        snapshot.bytesIn = stats.bytesIn;
        snapshot.bytesOut = stats.bytesOut;
        snapshot.localCalls = card->localCount();
        snapshot.printerTap = card->hasPrinterUnit();
        snapshot.printerBytes = card->bytesWritten();
        snapshot.printerOutranked = snapshot.printerTap &&
            (hasCard<PrinterCard>(bus) || hasCard<GrapplerCard>(bus));
        snapshot.hostClockCard = hasCard<ClockCard>(bus);
    }

    if (snapshot.lastError.empty()) snapshot.lastError = status_;
    return snapshot;
}

void NetworkCoordinator::applyFujiNetPanel(
    EmulationController& controller, const FujiNet_ImGui::Result& command)
{
    if (command.requestRescan) rescanSerialDevices();

    const bool hasLinkCommand = command.transportChanged ||
        command.portChanged || command.serialChanged ||
        command.timeoutChanged || command.requestStart ||
        command.requestStop || command.requestDropPeer;
    if (hasLinkCommand) {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = findFujiNet(bus);
        auto* link = card
            ? dynamic_cast<SpOverSlipLink*>(card->link()) : nullptr;
        if (link) {
            if (command.transportChanged) {
                switch (command.transportTo) {
                case FujiNet_ImGui::Transport::Tcp:
                    link->setTcpMode(link->tcpPort());
                    break;
                case FujiNet_ImGui::Transport::Serial:
                    link->setSerialMode(link->serialPath(), link->serialBaud());
                    break;
                case FujiNet_ImGui::Transport::Off:
                    link->setOff();
                    break;
                }
            }
            if (command.portChanged) link->setTcpMode(command.portTo);
            if (command.serialChanged) {
                link->setSerialMode(command.serialPathTo,
                                    command.serialBaudTo);
            }
            if (command.timeoutChanged) link->setTimeoutMs(command.timeoutTo);
            if (command.requestStart) {
                std::string error;
                if (!link->start(error)) status_ = std::move(error);
                else status_.clear();
            }
            if (command.requestStop) link->stop();
            if (command.requestDropPeer) {
                link->stop();
                std::string error;
                if (!link->start(error)) status_ = std::move(error);
            }
        }
    }

    // Helper-process and discovery commands are runtime-only and must never
    // wait behind the CPU's state mutex.
    if (command.helperPathChanged) setHelperPath(command.helperPathTo);
    if (command.requestHelperStart) startHelper();
    if (command.requestHelperStop) stopHelper();
    if (command.requestOpenWebUi)
        status_ = "FujiNet web UI: http://127.0.0.1/";
}

void NetworkCoordinator::rescanSerialDevices()
{
    serialDevices_.clear();
    for (const auto& device : SerialPort::enumerate())
        serialDevices_.emplace_back(device.path, device.description);
}

void NetworkCoordinator::setHelperPath(std::string path)
{
    helperPath_ = std::move(path);
    helperResolved_ = FujiNetHost::resolveHelper(helperPath_);
}

bool NetworkCoordinator::startHelper()
{
    std::string error;
    if (!host_->startHelper(helperPath_, error)) {
        status_ = std::move(error);
        return false;
    }
    status_.clear();
    return true;
}

void NetworkCoordinator::stopHelper()
{
    host_->stopHelper();
}

bool NetworkCoordinator::helperRunning()
{
    return host_->helperRunning();
}

int NetworkCoordinator::helperExitCode() const
{
    return host_->helperExitCode();
}

void NetworkCoordinator::shutdown(SpOverSlipLink* link)
{
    if (link) link->stop();
    if (host_) host_->stopHelper();
}

void NetworkCoordinator::shutdownFujiNet(EmulationController& controller)
{
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        if (auto* card = findFujiNet(bus)) {
            if (auto* link = dynamic_cast<SpOverSlipLink*>(card->link()))
                link->stop();
        }
    }
    if (host_) host_->stopHelper();
}

} // namespace pom2
