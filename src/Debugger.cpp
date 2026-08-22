// POM2 Apple II Emulator
// Run-control debugger — see Debugger.h for the design and the locking rule.

#include "Debugger.h"

#include <algorithm>

namespace pom2 {

namespace {
constexpr std::size_t kAddressSpace = 0x10000;
constexpr std::size_t kBpBytes      = kAddressSpace / 8;   // one bit each
}  // namespace

void Debugger::ensureBpBits()
{
    if (bpBits_.empty()) bpBits_.assign(kBpBytes, 0);
}

void Debugger::ensureWpBits()
{
    if (wpBits_.empty()) wpBits_.assign(kAddressSpace, 0);
}

// ── Breakpoints ──────────────────────────────────────────────────────────

void Debugger::addBreakpoint(uint16_t addr)
{
    ensureBpBits();
    uint8_t& byte = bpBits_[addr >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (addr & 7));
    if (byte & bit) return;                    // already set — keep the count honest
    byte = static_cast<uint8_t>(byte | bit);
    ++bpCount_;
}

void Debugger::removeBreakpoint(uint16_t addr)
{
    if (bpBits_.empty()) return;
    uint8_t& byte = bpBits_[addr >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (addr & 7));
    if (!(byte & bit)) return;
    byte = static_cast<uint8_t>(byte & ~bit);
    --bpCount_;
}

void Debugger::toggleBreakpoint(uint16_t addr)
{
    if (hasBreakpoint(addr)) removeBreakpoint(addr);
    else                     addBreakpoint(addr);
}

void Debugger::clearBreakpoints()
{
    // Freed rather than zeroed: an un-armed debugger should hold no memory,
    // and `armed()` keying off the count means the CPU goes back to its fast
    // loop the moment the last breakpoint goes.
    bpBits_.clear();
    bpBits_.shrink_to_fit();
    bpCount_ = 0;
}

bool Debugger::hasBreakpoint(uint16_t addr) const
{
    if (bpBits_.empty()) return false;
    return (bpBits_[addr >> 3] >> (addr & 7)) & 1u;
}

std::vector<uint16_t> Debugger::breakpoints() const
{
    std::vector<uint16_t> out;
    if (bpBits_.empty()) return out;
    out.reserve(bpCount_);
    for (std::size_t i = 0; i < kBpBytes; ++i) {
        uint8_t byte = bpBits_[i];
        while (byte) {
            const int bit = __builtin_ctz(byte);
            out.push_back(static_cast<uint16_t>(i * 8 + static_cast<std::size_t>(bit)));
            byte = static_cast<uint8_t>(byte & (byte - 1));
        }
    }
    return out;
}

// ── Watchpoints ──────────────────────────────────────────────────────────

void Debugger::setWatchpoint(uint16_t addr, Access access)
{
    if (access == None) {
        if (wpBits_.empty()) return;
        if (wpBits_[addr] != None) --wpCount_;
        wpBits_[addr] = None;
        if (wpCount_ == 0) { wpBits_.clear(); wpBits_.shrink_to_fit(); }
        return;
    }
    ensureWpBits();
    if (wpBits_[addr] == None) ++wpCount_;
    wpBits_[addr] = static_cast<uint8_t>(access);
}

Debugger::Access Debugger::watchpointAt(uint16_t addr) const
{
    if (wpBits_.empty()) return None;
    return static_cast<Access>(wpBits_[addr]);
}

void Debugger::clearWatchpoints()
{
    wpBits_.clear();
    wpBits_.shrink_to_fit();
    wpCount_ = 0;
}

std::vector<Debugger::Watch> Debugger::watchpoints() const
{
    std::vector<Watch> out;
    if (wpBits_.empty()) return out;
    out.reserve(wpCount_);
    for (std::size_t a = 0; a < kAddressSpace; ++a)
        if (wpBits_[a] != None)
            out.push_back({ static_cast<uint16_t>(a), static_cast<Access>(wpBits_[a]) });
    return out;
}

// ── Transient stops ──────────────────────────────────────────────────────

void Debugger::setTransient(uint16_t addr, Reason reason)
{
    transientAddr_   = addr;
    transientReason_ = reason;
    transientArmed_  = true;
}

void Debugger::clearTransient()
{
    transientArmed_  = false;
    transientReason_ = Reason::None;
}

// ── The hot hooks ────────────────────────────────────────────────────────

bool Debugger::onInstruction(uint16_t pc)
{
    // Resuming from a stop: the PC still points at the instruction that
    // stopped us, so without this the same breakpoint fires forever and Run
    // does nothing. One instruction of amnesty, cleared on use.
    if (resumeSkipArmed_ && pc == resumeSkip_) {
        resumeSkipArmed_ = false;
        return false;
    }
    resumeSkipArmed_ = false;

    if (transientArmed_ && pc == transientAddr_) {
        // Transients are one-shot by construction — step-over must not leave
        // a breakpoint behind at the return address.
        const Reason why = transientReason_;
        clearTransient();
        hit_ = { why, pc, 0, 0 };
        return true;
    }
    if (bpCount_ && hasBreakpoint(pc)) {
        hit_ = { Reason::Breakpoint, pc, 0, 0 };
        return true;
    }
    return false;
}

void Debugger::noteAccess(uint16_t addr, uint8_t value, bool write)
{
    if (wpBits_.empty()) return;
    const uint8_t want = write ? Write : Read;
    if (!(wpBits_[addr] & want)) return;
    // Do NOT overwrite an existing un-consumed hit: the first stop wins, and
    // an instruction that touches two watched addresses should report the one
    // that stopped it rather than the last one it happened to reach.
    if (hit_.valid()) return;
    hit_ = { write ? Reason::WatchWrite : Reason::WatchRead,
             /*pc=*/0, addr, value };
    // The PC is filled in by the controller when it parks: the access is
    // mid-instruction, and `programCounter` has already advanced past the
    // opcode, so a PC captured here would point at neither a useful place nor
    // a stable one.
}

void Debugger::clearHit()
{
    hit_ = {};
}

}  // namespace pom2
