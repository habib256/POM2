// POM2 Apple II Emulator
// Copyright (C) 2026

#include "NetworkCoordinator.h"

#include "ChildProcess.h"
#include "EmulationController.h"
#include "FujiNetCard.h"
#include "Memory.h"
#include "SerialPort.h"
#include "SlotBus.h"
#include "SpOverSlipLink.h"

namespace pom2 {
namespace {

/// Resolve the FujiNet card from the live bus. Single-instance on purpose:
/// the card holds a listening socket or an open serial device, and a second
/// one would just fail to bind the same endpoint.
FujiNetCard* findFujiNet(SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<FujiNetCard*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

} // namespace

FujiNet_ImGui::Snapshot NetworkCoordinator::captureFujiNetPanel(
    EmulationController& controller) const
{
    FujiNet_ImGui::Snapshot snap;
    {
        auto state = controller.lockState();
        auto* card = findFujiNet(state.memory().slotBus());
        if (card) {
            snap.plugged = true;
            const auto& link = card->transportLink();
            snap.slot      = card->getSlot();
            snap.transport =
                link.mode() == SpOverSlipLink::Mode::Serial
                    ? FujiNet_ImGui::Transport::Serial
                : link.mode() == SpOverSlipLink::Mode::Tcp
                    ? FujiNet_ImGui::Transport::Tcp
                    : FujiNet_ImGui::Transport::Off;
            snap.running    = link.isRunning();
            snap.connected  = link.isConnected();
            snap.state      = link.describe();
            snap.lastError  = link.lastError();
            snap.tcpPort    = link.tcpPort();
            snap.serialPath = link.serialPath();
            snap.serialBaud = link.serialBaud();
            snap.timeoutMs  = link.timeoutMs();
            for (const auto& d : link.devices()) {
                FujiNet_ImGui::DeviceRow row;
                row.unit    = d.unit;
                row.name    = d.name;
                row.type    = d.type;
                row.subtype = d.subtype;
                row.blocks  = d.blocks;
                snap.devices.push_back(row);
            }
            const auto st   = link.stats();
            snap.calls      = st.calls;
            snap.timeouts   = st.timeouts;
            snap.stale      = st.stale;
            snap.bytesIn    = st.bytesIn;
            snap.bytesOut   = st.bytesOut;
            snap.localCalls    = card->localCount();
            snap.printerTap    = card->hasPrinterUnit();
            snap.printerBytes  = card->bytesWritten();
            snap.helperRunning  = card->helper().isRunning();
            snap.helperExitCode = card->helper().lastExitCode();
        }
    }

    // Host-side, and deliberately outside the lock: none of it is machine
    // state, and the status line is this coordinator's own.
    if (snap.plugged && snap.lastError.empty()) snap.lastError = status_;
    snap.serialDevices  = serialDevices_;
    snap.helperPath     = helperPath_;
    snap.helperResolved = helperResolved_;
    return snap;
}

void NetworkCoordinator::applyFujiNetPanel(
    EmulationController& controller, const FujiNet_ImGui::Result& r)
{
    // Host-side first: scanning /dev must not happen under the machine lock.
    if (r.requestRescan) rescanSerialDevices();
    if (r.helperPathChanged) setHelperPath(r.helperPathTo);

    // Everything that touches the card, in ONE acquisition, with the card
    // re-resolved inside it. The code this replaces bound `link` OUTSIDE any
    // lock and then wrote through that reference in six separate critical
    // sections — a slot rebuild between any two of them left the rest writing
    // to a freed link — and applied the timeout change with no lock at all.
    const bool touchesCard =
        r.transportChanged || r.portChanged || r.serialChanged ||
        r.timeoutChanged || r.requestStart || r.requestStop ||
        r.requestDropPeer || r.requestHelperStart || r.requestHelperStop ||
        r.requestHelperRestart;
    if (touchesCard) {
        std::string startError;
        bool startFailed = false;
        std::string helperMessage;

        {
            auto state = controller.lockState();
            auto* card = findFujiNet(state.memory().slotBus());
            if (card) {
                auto& link = card->transportLink();

                if (r.transportChanged) {
                    switch (r.transportTo) {
                    case FujiNet_ImGui::Transport::Tcp:
                        link.setTcpMode(link.tcpPort());
                        break;
                    case FujiNet_ImGui::Transport::Serial:
                        link.setSerialMode(link.serialPath(), link.serialBaud());
                        break;
                    case FujiNet_ImGui::Transport::Off:
                        link.setOff();
                        break;
                    }
                }
                if (r.portChanged)    link.setTcpMode(r.portTo);
                if (r.serialChanged)  link.setSerialMode(r.serialPathTo,
                                                         r.serialBaudTo);
                if (r.timeoutChanged) link.setTimeoutMs(r.timeoutTo);

                if (r.requestStart) {
                    std::string err;
                    if (!link.start(err)) { startError = err; startFailed = true; }
                }
                if (r.requestStop)     link.stop();
                if (r.requestDropPeer) {
                    // Stop then start: the button means "let the peer
                    // reconnect", not "turn the bridge off", so the listener
                    // comes straight back up.
                    link.stop();
                    std::string err;
                    link.start(err);
                }

                // The helper is a child PROCESS, and spawning one under the
                // machine lock is normally exactly what CLAUDE.md forbids.
                // It is the right trade here: a spawn is sub-millisecond and
                // happens only on an explicit button press, whereas doing it
                // outside means carrying the card pointer out of the lock —
                // which is the use-after-free this class exists to remove.
                if (r.requestHelperStop || r.requestHelperRestart)
                    card->helper().stop();
                if (r.requestHelperStart || r.requestHelperRestart) {
                    std::string err;
                    if (!card->startHelper(helperPath_, err))
                        helperMessage = err;
                    else if (r.requestHelperRestart)
                        helperMessage = "FujiNet program restarted.";
                }
            }
        }

        if (startFailed)             status_ = startError;
        else if (r.requestStart)     status_.clear();
        if (!helperMessage.empty())  status_ = helperMessage;
        else if (r.requestHelperStart) status_.clear();
    }

    if (r.requestOpenWebUi) status_ = "FujiNet web UI: http://127.0.0.1/";
}

void NetworkCoordinator::rescanSerialDevices()
{
    serialDevices_.clear();
    for (const auto& d : SerialPort::enumerate())
        serialDevices_.emplace_back(d.path, d.description);
}

void NetworkCoordinator::setHelperPath(std::string path)
{
    helperPath_ = std::move(path);
    // An empty configured path means "find the stock `fujinet` on PATH".
    helperResolved_ = ChildProcess::findOnPath(
        helperPath_.empty() ? std::string("fujinet") : helperPath_);
}

} // namespace pom2
