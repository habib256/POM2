// POM2 frontend concurrency contract.
//
// This is deliberately renderer-free: ImGui itself is single-threaded, while
// the risky boundary is the frame host taking snapshots/applying commands as
// the CPU worker and an AI-like state reader run.  The test also replugs the
// exact cards being inspected, which catches cached raw-pointer lifetime bugs
// particularly well under -fsanitize=thread.

#include "AudioCoordinator.h"
#include "ClockCard.h"
#include "DevicePanelCoordinator.h"
#include "CffaCard.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "EchoPlusCard.h"
#include "FujiNetCard.h"
#include "LeChatMauveCard.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseCoordinator.h"
#include "NetworkCoordinator.h"
#include "PrinterCoordinator.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "PhasorCard.h"
#include "GrapplerCard.h"
#include "Settings.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SpOverSlipLink.h"
#include "StorageCoordinator.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace {

std::unique_ptr<pom2::FujiNetCard> makeFujiNetCard(int slot)
{
    auto card = std::make_unique<pom2::FujiNetCard>(slot);
    auto link = std::make_unique<pom2::SpOverSlipLink>();
    link->setOff();
    card->setLink(std::move(link));
    return card;
}

void plugInitialCards(EmulationController& controller)
{
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    bus.plug(4, std::make_unique<pom2::UthernetCard>(4));
    bus.plug(5, std::make_unique<pom2::UthernetIICard>(5));
    bus.plug(7, std::make_unique<LeChatMauveCard>(7));
    bus.plug(3, std::make_unique<pom2::SmartPortCard>(3));
    bus.plug(2, makeFujiNetCard(2));
    bus.plug(1, std::make_unique<SuperSerialCard>(1));
    bus.plug(6, std::make_unique<PrinterCard>(6));
}

} // namespace

int main()
{
    EmulationController controller;
    pom2::Settings settings;
    settings.setReadOnly(true);
    pom2::DevicePanelCoordinator panels(controller, settings);
    pom2::AudioCoordinator audio(controller.audio(), controller);
    pom2::MouseCoordinator mouse(controller);
    pom2::NetworkCoordinator network;
    pom2::PrinterCoordinator printer;
    pom2::SlotConfigurationCoordinator slots;
    pom2::StorageCoordinator storage;
    slots.resolve(settings, pom2::SystemProfile::AppleIIe);
    plugInitialCards(controller);

    const auto initial = panels.captureInventory();
    assert(initial.chatMauveSlot == 7);
    assert(initial.uthernetSlot == 4);
    assert(initial.uthernetIISlot == 5);
    assert(initial.smartPortSlot == 3);
    assert(initial.fujiNetSlot == 2);
    assert(initial.serialSlots == std::vector<int>{1});
    assert(initial.printerSlot == 6);

    controller.setCyclesPerFrame(4096);
    controller.start();
    controller.setMode(EmulationController::Mode::Running);

    std::atomic<bool> keepPolling{true};
    std::atomic<std::uint64_t> aiPolls{0};
    std::thread aiReader([&] {
        while (keepPolling.load(std::memory_order_relaxed)) {
            auto state = controller.lockState();
            const auto video = state.memory().getDisplayState();
            (void)video;
            aiPolls.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    for (int frame = 0; frame < 600; ++frame) {
        const auto inventory = panels.captureInventory();
        const auto ethernet = panels.captureEthernet();
        const auto chat = panels.captureChatMauve();
        const auto fujiNet = network.captureFujiNetPanel(controller);
        const auto smartPort = storage.captureSmartPortPanel(controller);
        const auto storageInventory = storage.captureInventory(controller);
        const auto audioInventory = audio.captureInventory();
        const auto audioMixer = audio.captureMixerCards();
        const auto mouseSnapshot = mouse.capture();
        const auto serial = panels.captureSerialCards();
        const auto printerPanel = printer.capturePrinterPanel(controller);
        const auto printerHost = printer.captureHost(controller);
        pom2::SlotConfigurationCoordinator::LiveSnapshot liveSlots;
        {
            auto state = controller.lockState();
            liveSlots = slots.captureLive(state.memory().slotBus());
        }

        assert(inventory.uthernetIISlot == 5);
        assert(liveSlots.plugged(4));
        assert(slots.effectivePlan()[4] == "mockingboard");
        assert(ethernet.u2Plugged && ethernet.u2Slot == 5);
        assert(chat.plugged == inventory.chatMauvePlugged());
        if (smartPort.plugged) assert(smartPort.slot == 3);
        if (storageInventory.hasDiskII())
            assert(storageInventory.primaryDiskIISlot == 6);
        if (storageInventory.hasBlockDevice())
            assert(storageInventory.blockSlots == std::vector<int>{6});
        if (storageInventory.hasSmartPort())
            assert(storageInventory.primarySmartPortSlot == 3);
        if (fujiNet.plugged) assert(fujiNet.slot == 2);
        if (!serial.empty()) assert(serial.front().slot == 1);
        if (inventory.printerPlugged()) {
            assert(printerPanel.plugged && printerPanel.slot == 6);
            assert(printerHost.source ==
                   pom2::PrinterCoordinator::SourceKind::PrinterCard);
        }
        for (const auto& card : audioMixer) {
            pom2::AudioCoordinator::MixerCardCommand audioCommand;
            audioCommand.kind = card.kind;
            audioCommand.slot = card.slot;
            audioCommand.volume = (frame & 1) ? 0.4f : 0.6f;
            audioCommand.muted = (frame % 7) == 0;
            assert(audio.applyMixerCard(audioCommand));
        }
        if (audioInventory.hasMockingboard())
            assert(audio.captureMockingboard().plugged);
        if (audioInventory.hasPhasor())
            assert(audio.capturePhasor().plugged);
        if (audioInventory.hasEchoPlus())
            assert(audio.captureEchoPlus().plugged);
        if (mouseSnapshot.plugged())
            assert(mouse.routeHost(static_cast<std::uint8_t>(frame),
                                   static_cast<std::uint8_t>(frame * 3),
                                   (frame & 1) != 0) > 0);
        if (inventory.clockPlugged()) assert(inventory.clockSlot == 4);

        pom2::Uthernet_ImGui::FrameResult ethernetCommand;
        ethernetCommand.requestVirtualDns = true;
        ethernetCommand.virtualDnsTo = (frame & 1) == 0;
        if ((frame % 31) == 0) ethernetCommand.requestResetU2 = true;
        panels.applyEthernet(ethernetCommand);

        pom2::LeChatMauve_ImGui::FrameResult chatCommand;
        chatCommand.requestInvertBit7 = true;
        chatCommand.invertBit7To = (frame & 1) != 0;
        chatCommand.requestColorTextEnable = true;
        chatCommand.colorTextEnableTo = (frame % 3) != 0;
        chatCommand.requestHgrDuochrome = true;
        chatCommand.hgrDuochromeTo = (frame % 5) == 0;
        panels.applyChatMauve(chatCommand);

        if (!serial.empty()) {
            pom2::DevicePanelCoordinator::SerialCommand serialCommand;
            serialCommand.slot = serial.front().slot;
            serialCommand.requestRawMode = true;
            serialCommand.rawMode = (frame & 1) != 0;
            serialCommand.requestPrinterTap = true;
            serialCommand.printerTap = (frame % 3) == 0;
            const auto result = panels.applySerial(serialCommand);
            assert(result.cardFound);
        }

        (void)printer.drainImageWriter(controller);
        if (printerHost.grapplerPlugged) {
            (void)printer.setGrapplerPrinterType(controller, frame % 7);
            (void)printer.setGrapplerBusy(controller, (frame & 1) != 0);
        }

        if (fujiNet.plugged) {
            pom2::FujiNet_ImGui::Result fujiNetCommand;
            fujiNetCommand.timeoutChanged = true;
            fujiNetCommand.timeoutTo = 50 + (frame % 200);
            if ((frame % 29) == 0) {
                fujiNetCommand.transportChanged = true;
                fujiNetCommand.transportTo =
                    pom2::FujiNet_ImGui::Transport::Off;
            }
            network.applyFujiNetPanel(controller, fujiNetCommand);
        }

        pom2::SmartPort_ImGui::Result smartPortCommand;
        if (smartPort.plugged) {
            if (smartPort.units[0].kind.empty()) {
                smartPortCommand.units[0].setType = "35";
            } else if ((frame % 17) == 0) {
                smartPortCommand.units[0].writeBackChanged = true;
                smartPortCommand.units[0].writeBackOn = (frame & 1) != 0;
            }
            (void)storage.applySmartPortPanel(
                controller, settings, smartPort.slot, smartPortCommand);
        }

        // Slot Config's Apply destroys and recreates cards. A coordinator
        // which retained one of the old raw pointers would race/use-after-free
        // here; resolving through SlotBus under lock keeps this safe.
        if ((frame % 25) == 0) {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(4);
            switch ((frame / 25) % 4) {
                case 0:
                    bus.plug(4, std::make_unique<pom2::UthernetCard>(4));
                    break;
                case 1:
                    bus.plug(4, std::make_unique<MouseCard>(4));
                    break;
                case 2:
                    bus.plug(4, std::make_unique<MouseCardAppleWin>(4));
                    break;
                default:
                    bus.plug(4, std::make_unique<ClockCard>(4));
                    break;
            }
        }
        if ((frame % 40) == 0) {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(7);
            if ((frame % 80) != 0)
                bus.plug(7, std::make_unique<LeChatMauveCard>(7));
        }
        if ((frame % 60) == 0) {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(3);
            if ((frame % 120) != 0)
                bus.plug(3, std::make_unique<pom2::SmartPortCard>(3));
        }
        if ((frame % 75) == 0) {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(2);
            if ((frame % 150) != 0) bus.plug(2, makeFujiNetCard(2));
        }
        if ((frame % 45) == 0) {
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(1);
            if ((frame % 90) != 0)
                bus.plug(1, std::make_unique<SuperSerialCard>(1));
        }
        if ((frame % 55) == 0) {
            printer.resetFeedCursor();
            auto state = controller.lockState();
            auto& bus = state.memory().slotBus();
            (void)bus.unplug(6);
            switch ((frame / 55) % 8) {
                case 0:
                    bus.plug(6, std::make_unique<GrapplerCard>(6));
                    break;
                case 1:
                    bus.plug(6, std::make_unique<PrinterCard>(6));
                    break;
                case 2:
                    bus.plug(6, std::make_unique<DiskIICard>(6));
                    break;
                case 3:
                    bus.plug(6, std::make_unique<ProDOSHardDiskCard>(6));
                    break;
                case 4:
                    bus.plug(6, std::make_unique<pom2::CffaCard>(6));
                    break;
                case 5:
                    bus.plug(6, std::make_unique<MockingboardCard>(6));
                    break;
                case 6:
                    bus.plug(6, std::make_unique<PhasorCard>(6));
                    break;
                default:
                    bus.plug(6, std::make_unique<EchoPlusCard>(6));
                    break;
            }
        }
        std::this_thread::yield();
    }

    keepPolling.store(false, std::memory_order_relaxed);
    aiReader.join();
    controller.stop();

    assert(aiPolls.load(std::memory_order_relaxed) > 0);
    assert(settings.getBool("uthernet2_virtual_dns", false) == false);
    std::cout << "frontend device panel concurrency: OK (AI polls="
              << aiPolls.load(std::memory_order_relaxed) << ")\n";
    return 0;
}
