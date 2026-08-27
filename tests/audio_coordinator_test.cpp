// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// AudioCoordinator slot-topology and immutable-snapshot contract.

#include "AudioCoordinator.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "EmulationController.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SlotBus.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool near(float lhs, float rhs)
{
    return std::fabs(lhs - rhs) < 0.0001f;
}

} // namespace

int main()
{
    EmulationController controller;
    pom2::AudioCoordinator audio(controller.audio(), controller);
    pom2::Settings settings;
    pom2::PrinterSoundDevice printer;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();

        auto mb2 = std::make_unique<MockingboardCard>(
            2, MockingboardCard::Variant::AC);
        mb2->setVolume(0.25f);
        mb2->setMuted(true);
        bus.plug(2, std::move(mb2));

        auto echo = std::make_unique<EchoPlusCard>(3);
        echo->setVolume(0.35f);
        bus.plug(3, std::move(echo));

        auto phasor = std::make_unique<PhasorCard>(4);
        phasor->setVolume(0.45f);
        bus.plug(4, std::move(phasor));

        bus.plug(5, std::make_unique<EchoPlusTMS5220Card>(5));

        auto mb6 = std::make_unique<MockingboardCard>(
            6, MockingboardCard::Variant::SoundII);
        mb6->setVolume(0.65f);
        bus.plug(6, std::move(mb6));
    }

    const auto inventory = audio.captureInventory();
    assert(inventory.mockingboardSlots == std::vector<int>({2, 6}));
    assert(inventory.phasorSlots == std::vector<int>({4}));
    assert(inventory.echoPlusSlots == std::vector<int>({3}));
    assert(inventory.echoPlusTmsSlots == std::vector<int>({5}));
    assert(inventory.primaryMockingboardSlot() == 6);

    const auto mixer = audio.captureMixerCards();
    assert(mixer.size() == 4); // TMS scaffold has no AudioSource yet
    assert(mixer[0].slot == 2 && near(mixer[0].volume, 0.25f));
    assert(mixer[3].slot == 6 && near(mixer[3].volume, 0.65f));

    const auto mb = audio.captureMockingboard();
    assert(mb.plugged && mb.slot == 6 && mb.hasSsi);
    const auto ph = audio.capturePhasor();
    assert(ph.plugged && ph.slot == 4);
    const auto ep = audio.captureEchoPlus();
    assert(ep.plugged && ep.slot == 3);

    pom2::AudioCoordinator::MixerCardCommand command;
    command.kind = pom2::AudioCoordinator::CardKind::Mockingboard;
    command.slot = 2;
    command.volume = 0.75f;
    command.muted = false;
    assert(audio.applyMixerCard(command));
    const auto changed = audio.captureMixerCards();
    assert(near(changed[0].volume, 0.75f));
    assert(!changed[0].muted);

    audio.persist(settings,
                  controller.speaker(), controller.cassette(),
                  controller.floppySound525(), controller.floppySound35(),
                  printer);
    assert(near(settings.getFloat("mockingboard_slot2_volume"), 0.75f));
    assert(near(settings.getFloat("mockingboard_slot6_volume"), 0.65f));
    // Compatibility key follows the former last-plugged/highest-slot policy.
    assert(near(settings.getFloat("mockingboard_volume"), 0.65f));

    settings.setFloat("phasor_volume", 0.5f);
    settings.setFloat("phasor_slot4_volume", 0.9f);
    settings.setBool("phasor_slot4_muted", true);
    const auto restored = audio.restoreCardSettings(
        settings, pom2::AudioCoordinator::CardKind::Phasor, 4, 0.1f);
    assert(near(restored.volume, 0.9f));
    assert(restored.muted);

    // Reuse the old primary slot for a different type. Any cached alias would
    // now be stale; the coordinator instead reports the live bus topology.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(6);
        bus.plug(6, std::make_unique<EchoPlusCard>(6));
    }
    const auto rebuilt = audio.captureInventory();
    assert(rebuilt.mockingboardSlots == std::vector<int>({2}));
    assert(rebuilt.echoPlusSlots == std::vector<int>({3, 6}));
    assert(audio.captureMockingboard().slot == 2);
    assert(audio.captureEchoPlus().slot == 6);
    assert(audio.applyMixerCard(command)); // slot 2 is still a Mockingboard

    command.slot = 6; // but slot 6 no longer matches the command's kind
    assert(!audio.applyMixerCard(command));

    std::cout << "audio coordinator: OK\n";
    return 0;
}
