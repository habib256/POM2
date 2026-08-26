// Audio-source teardown inventory smoke test.
//
// Pins the 2026-08-02 use-after-free: "mockingboard" (Variant::AC) and
// "mockingboard_c" (Variant::SoundII) are DISTINCT catalog keys, so the Slot
// Config uniqueness rule (which compares exact key strings) happily plugs
// both — slot 4 = A/C, slot 5 = Sound II. MainWindow registered each card's
// AudioSource with the AudioDevice but remembered only ONE raw alias
// (`mockingboardCard`, last one wins), and teardown removed exactly that one
// source before `slotBus().clear()` destroyed BOTH cards. The audio callback
// thread then dereferenced the survivor every ~5 ms.
//
// The fix is structural: AudioCoordinator owns the inventory of every source
// registered with AudioDevice and drains it before slot cards are destroyed.
// This test exercises that production coordinator against the real cards and
// the real AudioDevice:
//
//   1. The two variants really are two coexisting cards on one bus, with two
//      DISTINCT AudioSource pointers — no aliasing saves the alias-based
//      teardown.
//   2. Removing one source leaves the other REGISTERED (i.e. still called by
//      mixSources), and only an inventory-shaped teardown empties the device.
//
// Registration oracle: AudioDevice::mixSources writes `lastBufferPeak` on
// every source it visits (AudioDevice.cpp:138-142), and never on one it does
// not. Parking a sentinel there and checking whether mixSources disturbed it
// answers "is this source still registered?" without any accessor.

#include "AudioDevice.h"
#include "AudioCoordinator.h"
#include "Mockingboard.h"
#include "SlotBus.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr float kSentinel = 4.0f;   // out of any real peak's range

// True when `mixSources` touched this source, i.e. it is still registered.
bool stillRegistered(AudioDevice& dev, AudioSource* src)
{
    src->lastBufferPeak.store(kSentinel, std::memory_order_relaxed);
    std::vector<float> out(256 * AudioDevice::kChannels, 0.0f);
    dev.mixSources(out.data(), 256);
    return src->lastBufferPeak.load(std::memory_order_relaxed) != kSentinel;
}

}  // namespace

int main()
{
    AudioDevice dev;
    SlotBus     bus;

    // Exactly what Slot Config → slot 4 "Mockingboard A/C" + slot 5
    // "Mockingboard C (Sound II)" → Apply builds.
    auto ac  = std::make_unique<MockingboardCard>(4, MockingboardCard::Variant::AC);
    auto s2  = std::make_unique<MockingboardCard>(5, MockingboardCard::Variant::SoundII);
    MockingboardCard* acRaw = ac.get();
    MockingboardCard* s2Raw = s2.get();

    AudioSource* acSrc = acRaw->audioSource();
    AudioSource* s2Src = s2Raw->audioSource();
    assert(acSrc && s2Src);
    // (1) Two cards, two sources. A single `mockingboardCard` alias cannot
    //     name both, which is the whole bug.
    assert(acSrc != s2Src);

    bus.plug(4, std::move(ac));
    bus.plug(5, std::move(s2));
    assert(bus.peripheral(4) == acRaw);
    assert(bus.peripheral(5) == s2Raw);

    pom2::AudioCoordinator audio(dev);
    for (AudioSource* s : { acSrc, s2Src }) {
        audio.registerSource(s);
    }
    audio.registerSource(acSrc); // idempotent
    assert(audio.sourceCount() == 2);
    assert(stillRegistered(dev, acSrc));
    assert(stillRegistered(dev, s2Src));

    // (2) The old alias-based teardown: one removeSource for the one alias
    //     (which pointed at the LAST card plugged). The other card's source
    //     stays live in the device — that is the dangling pointer the audio
    //     thread called through after slotBus().clear().
    dev.removeSource(s2Src); // deliberately bypass the coordinator
    assert(!stillRegistered(dev, s2Src));
    assert(stillRegistered(dev, acSrc));

    // The inventory teardown drains everything, whatever the aliases say.
    audio.unregisterAll();
    assert(audio.sourceCount() == 0);
    assert(!stillRegistered(dev, acSrc));
    assert(!stillRegistered(dev, s2Src));

    // Only now may the bus destroy the cards. A mix after the clear must not
    // reach either source (under ASan a regression here is a hard failure;
    // without it, at least the sentinel oracle above has already proven the
    // device list is empty).
    bus.clear();
    std::vector<float> out(256 * AudioDevice::kChannels, 1.0f);
    dev.mixSources(out.data(), 256);
    for (float v : out) assert(v == 0.0f);

    std::printf("audio_source_teardown_test: OK\n");
    return 0;
}
