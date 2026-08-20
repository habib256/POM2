// Snapshot-blob parser fuzz smoke — bounded, deterministic, self-contained.
//
// A snapshot is untrusted input in the same way a disk image is: the user
// loads a `.snap` from wherever they got it, and the AI control server accepts
// one over HTTP. `restoreMachineState` walks a section list driven by lengths
// that came out of that blob, and `MachineSnapshot.h` already records one
// past over-read here ("the round 10 #3 over-read hardening") — which is the
// reason to keep testing it rather than assume it stays fixed.
//
// The blob is CAPTURED, not invented: a random buffer is rejected at the magic
// and never reaches the section walker, so a fuzzer seeded with noise would
// only ever exercise the reject path. Mutating a real capture keeps the header
// plausible and gets the fuzzer through the front door — the test prints the
// acceptance rate so a change that starts rejecting everything (and therefore
// testing nothing) is visible rather than silent.
//
// Mutations are STRUCTURE-AWARE, which is what makes this test bite. Blind
// byte-flipping cannot find a section-length bug: the length fields are four
// bytes each in a ~160 KB blob, so a random flip essentially never lands on
// one. `mutateSections` walks the section list (name[8] + len[4] + payload)
// and aims at the lengths and names directly. Verified against a deliberately
// removed bounds check in `Memory::loadSnapshotState` — the blind version
// missed it entirely, the structure-aware version catches it.
//
// Note what this can and cannot prove. Every read in `SnapshotReader` goes
// through an istream over a bounded streambuf, so THAT layer cannot over-read
// by construction; its guards exist to stop unbounded ALLOCATION. The real
// raw-pointer parser downstream is `Memory::loadSnapshotState`, reached via
// the MEX section, and shortening MEX's declared length is exactly how you
// would catch a missing check there.
//
// Restore is TRANSACTIONAL for file/API input, so a mutant that fails partway
// must leave the machine as it was; the loop reads through CPU and memory
// afterwards, since a restore that reports success but leaves an inconsistent
// Memory only shows up when something uses it.
//
// Under a plain build this catches crashes and assertion failures. Under
// `-fsanitize=address,undefined` it also catches any over-read the section
// walker could be talked into.

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr size_t kMagicLen  = 8;
constexpr size_t kHeaderLen = kMagicLen + 4 + 4;   // magic + version + reserved
constexpr size_t kNameLen   = 8;

uint32_t rd32(const uint8_t* p)
{ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void wr32(uint8_t* p, uint32_t v)
{ for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

/// Aim at the section table rather than at the bytes. Returns false when the
/// blob no longer has a walkable one (a previous mutation may have wrecked it).
bool mutateSections(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.size() < kHeaderLen + kNameLen + 4) return false;

    std::vector<size_t> hdrs;                       // offset of each name[8]
    for (size_t i = kHeaderLen; i + kNameLen + 4 <= b.size();) {
        const uint32_t len = rd32(b.data() + i + kNameLen);
        const size_t payload = i + kNameLen + 4;
        if (len > b.size() - payload) break;
        hdrs.push_back(i);
        i = payload + len;
    }
    if (hdrs.empty()) return false;

    const size_t h = hdrs[rng() % hdrs.size()];
    switch (rng() % 3) {
        case 0:     // SHORTEN a section — the case that reaches a downstream
                    // raw-pointer parser with less data than it declared.
            wr32(b.data() + h + kNameLen, uint32_t(rng() % 4096));
            break;
        case 1:     // Overlong / absurd length — allocation and EOF guards.
            wr32(b.data() + h + kNameLen,
                 (rng() % 2) ? 0xFFFFFFFFu : uint32_t(rng()));
            break;
        case 2:     // Corrupt the NAME so a section is misrouted, or rename a
                    // neighbour onto MEX/MEM and feed it foreign bytes.
            b[h + (rng() % kNameLen)] = uint8_t(rng());
            break;
    }
    return true;
}

void mutate(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.empty()) return;
    switch (rng() % 5) {
        case 0: {                                  // truncate mid-section
            std::uniform_int_distribution<size_t> d(1, b.size());
            b.resize(d(rng));
            break;
        }
        case 1: {                                  // smash a length field
            if (b.size() < 4) break;
            std::uniform_int_distribution<size_t> d(0, b.size() - 4);
            const size_t off = d(rng);
            for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(rng());
            break;
        }
        case 2: {                                  // max a length — overflow bait
            if (b.size() < 8) break;
            std::uniform_int_distribution<size_t> d(0, b.size() - 8);
            const size_t off = d(rng);
            for (int i = 0; i < 8; ++i) b[off + i] = 0xFF;
            break;
        }
        case 3: {                                  // scatter
            std::uniform_int_distribution<size_t> d(0, b.size() - 1);
            const int n = 1 + int(rng() % 24);
            for (int i = 0; i < n; ++i) b[d(rng)] = uint8_t(rng());
            break;
        }
        case 4: {                                  // trailing garbage
            const size_t add = rng() % 1024;
            for (size_t i = 0; i < add; ++i) b.push_back(uint8_t(rng()));
            break;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const unsigned seed  = (argc > 1) ? unsigned(std::stoul(argv[1])) : 20260820u;
    const int      iters = (argc > 2) ? std::stoi(argv[2]) : 1500;

    // One real capture to mutate from.
    std::vector<uint8_t> golden;
    {
        Memory mem;
        M6502  cpu(&mem);
        for (int i = 0; i < 4096; ++i)
            mem.writeRamUnchecked(uint16_t(i), uint8_t(i * 7));
        pom2::SnapshotWriter w(golden);
        pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
        assert(w.finish() && "capture must succeed");
    }
    assert(!golden.empty());

    // The unmutated blob MUST restore, or every mutant below is only
    // exercising the reject path and the test proves nothing.
    {
        Memory mem; M6502 cpu(&mem);
        pom2::SnapshotReader r(golden.data(), golden.size());
        const auto res = pom2::restoreMachineState(r, cpu, mem);
        assert(res.ok && "golden snapshot must round-trip");
    }

    std::mt19937 rng(seed);
    int accepted = 0;
    for (int i = 0; i < iters; ++i) {
        std::vector<uint8_t> b = golden;
        const int rounds = 1 + int(rng() % 3);
        for (int r = 0; r < rounds; ++r) {
            // Mostly structure-aware; the generic pass still runs sometimes,
            // since it is what produces the ragged truncations and broken
            // magics the front door has to reject.
            if (!(rng() % 4) || !mutateSections(b, rng)) mutate(b, rng);
        }
        if (b.empty()) continue;

        Memory mem; M6502 cpu(&mem);
        pom2::SnapshotReader rd(b.data(), b.size());
        const auto res = pom2::restoreMachineState(rd, cpu, mem);
        if (res.ok) ++accepted;
        // Use the machine: an "ok" restore that left Memory inconsistent is
        // only visible through a read.
        for (int a = 0; a < 64; ++a) (void)mem.memRead(uint16_t(a * 1021));
        (void)cpu.getProgramCounter();
    }

    // Guard the fuzzer against itself: if mutants stop being accepted, the
    // walker is no longer being reached and this test has quietly become a
    // no-op. Loose bound — it is a smoke alarm, not a tuned ratio.
    assert(accepted > iters / 20 &&
           "too few mutants accepted — the section walker is not being reached");

    std::printf("fuzz_snapshot: %d mutants survived, %d accepted (seed %u)\n",
                iters, accepted, seed);
    return 0;
}
