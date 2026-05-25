// SpeakerDevice event-overflow parity regression test.
//
// The Apple II speaker is a 1-bit flip-flop: each recorded toggle is a PARITY
// flip, not an absolute level. When the event queue overflowed, the trim
// dropped a single (oldest) toggle, which inverts the reconstructed level of
// every later sample — an audible click/polarity glitch rather than just lost
// time. The trim now drops toggles in PAIRS so parity is preserved.
//
// Observable difference: push an ODD number of toggles, far more than the
// queue cap. With the single-drop bug the steady-state retained count equals
// the (even) cap; with the pair-drop fix it settles one below the cap (odd).
// We assert the queue actually trimmed (count < pushed) AND the retained
// count is odd — which fails on the old single-drop behaviour.

#include "SpeakerDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main()
{
    SpeakerDevice spk;
    spk.reset();

    // Odd, and comfortably larger than kMaxEvents (private constant = 16384).
    constexpr uint64_t kPushed = 200001;
    for (uint64_t i = 0; i < kPushed; ++i)
        spk.recordToggle(i + 1);   // strictly increasing CPU cycle stamps

    const size_t count = spk.getQueuedEventCount();

    // The queue must have actually overflowed and trimmed (guards against a
    // future cap ≥ kPushed silently making the parity check vacuous).
    assert(count < kPushed && "queue should have trimmed on overflow");
    // Pair-drop ⇒ parity preserved ⇒ retained count is odd for an odd push
    // count. The old single-drop trim pinned the count at the even cap.
    assert((count & 1u) == 1u && "overflow trim must drop toggles in pairs");

    std::printf("OK speaker_overflow (pair-drop preserves toggle parity, n=%zu)\n",
                count);
    return 0;
}
