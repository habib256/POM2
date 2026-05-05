// Smoke test for the paste-text → keyboard pipeline. Pins:
//   - line-ending normalisation (\r\n / \r / \n all → one CR)
//   - unprintable controls below $20 dropped (except CR / HT)
//   - high bit stripped (Apple II is 7-bit)
//   - queue drains exactly one byte per $C010 strobe clear
//   - cap at Memory::kPasteMaxChars (no overflow)

#include "Memory.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>

namespace {

// Read $C000 + clear strobe via $C010, returning the latched byte (low 7
// bits) and the strobe state both before and after the clear. Mimics the
// Monitor's KEYIN flow.
uint8_t consumeKey(Memory& mem)
{
    const uint8_t latched = mem.memRead(0xC000);
    assert(latched & 0x80);                  // strobe must be high
    (void)mem.memRead(0xC010);               // clear strobe — drains queue
    return latched & 0x7F;
}

bool keyReady(Memory& mem) { return (mem.memRead(0xC000) & 0x80) != 0; }

} // namespace

int main()
{
    // ── Empty paste is a no-op ───────────────────────────────────────────
    {
        Memory mem;
        const size_t queued = mem.pasteText("");
        assert(queued == 0);
        assert(!keyReady(mem));
        assert(mem.pendingPasteSize() == 0);
    }

    // ── Single line, plain ASCII, no line endings ────────────────────────
    {
        Memory mem;
        const size_t queued = mem.pasteText("PRINT 1");
        assert(queued == 7);
        // First byte is in the latch, the other 6 in the queue.
        assert(mem.pendingPasteSize() == 6);
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "PRINT 1");
    }

    // ── \r\n normalisation: "A\r\nB" → "A\rB" (one CR, no double LF) ─────
    {
        Memory mem;
        mem.pasteText("A\r\nB");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out.size() == 3);
        assert(out[0] == 'A');
        assert(out[1] == 0x0D);
        assert(out[2] == 'B');
    }

    // ── \n alone also becomes CR ─────────────────────────────────────────
    {
        Memory mem;
        mem.pasteText("X\nY");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "X\rY");
    }

    // ── Bare \r becomes CR (no LF following) ─────────────────────────────
    {
        Memory mem;
        mem.pasteText("X\rY");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "X\rY");
    }

    // ── Controls below $20 dropped (except CR/HT) ────────────────────────
    {
        Memory mem;
        std::string in;
        in.push_back('A');
        in.push_back(0x07);  // BEL — drop
        in.push_back('B');
        in.push_back(0x09);  // HT — keep
        in.push_back('C');
        in.push_back(0x1B);  // ESC — drop
        in.push_back('D');
        const size_t queued = mem.pasteText(in);
        assert(queued == 5);   // A B HT C D
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out.size() == 5);
        assert(out[0] == 'A' && out[1] == 'B' && out[2] == 0x09 &&
               out[3] == 'C' && out[4] == 'D');
    }

    // ── High bit stripped ────────────────────────────────────────────────
    {
        Memory mem;
        std::string in;
        in.push_back(static_cast<char>(0xC1));  // 'A' with bit 7 set
        mem.pasteText(in);
        assert(consumeKey(mem) == 'A');
    }

    // ── Cap at Memory::kPasteMaxChars ────────────────────────────────────
    {
        Memory mem;
        std::string huge(Memory::kPasteMaxChars + 100, 'X');
        const size_t queued = mem.pasteText(huge);
        assert(queued == Memory::kPasteMaxChars);
    }

    // ── cancelPaste empties the queue but doesn't clear the latch ────────
    {
        Memory mem;
        mem.pasteText("HELLO");
        assert(mem.pendingPasteSize() == 4);  // first byte in latch
        mem.cancelPaste();
        assert(mem.pendingPasteSize() == 0);
        // Latch still has the first byte ready until the strobe is cleared.
        assert(keyReady(mem));
        (void)mem.memRead(0xC010);
        // Now the latch should be empty (queue was cancelled, no replenish).
        assert(!keyReady(mem));
    }

    std::printf("Paste smoke: OK (line endings, controls, 7-bit, cap, cancel)\n");
    return 0;
}
