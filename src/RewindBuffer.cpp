// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// RewindBuffer — see RewindBuffer.h.

#include "RewindBuffer.h"

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cstring>

namespace pom2 {
namespace {

// ─── XOR delta codec ──────────────────────────────────────────────────────
// A delta encodes (a -> b) for two EQUAL-length blobs as a sequence of
// records: [u32 offset][u32 length][length XOR bytes]. Only spans where the
// blobs differ are stored; short equal gaps inside a changed region are
// coalesced into one record (an 8-byte header costs more than a few XOR-zero
// bytes). XOR is its own inverse, so applying a delta to `a` yields `b` and
// applying it again yields `a` — handy for bidirectional scrubbing.

constexpr size_t kCoalesceGap = 16;   // bridge equal gaps shorter than this

void appendU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint32_t readU32(const std::vector<uint8_t>& in, size_t at)
{
    return static_cast<uint32_t>(in[at])
         | (static_cast<uint32_t>(in[at + 1]) << 8)
         | (static_cast<uint32_t>(in[at + 2]) << 16)
         | (static_cast<uint32_t>(in[at + 3]) << 24);
}

void encodeXorDelta(const std::vector<uint8_t>& a,
                    const std::vector<uint8_t>& b,
                    std::vector<uint8_t>& out)
{
    out.clear();
    const size_t n = a.size();   // caller guarantees a.size() == b.size()
    size_t i = 0;
    while (i < n) {
        while (i < n && a[i] == b[i]) ++i;
        if (i >= n) break;
        const size_t spanStart = i;
        size_t spanEnd = i + 1;          // exclusive; tracks last differing byte + 1
        size_t j = i + 1;
        while (j < n) {
            if (a[j] != b[j]) { spanEnd = j + 1; ++j; }
            else if (j - spanEnd >= kCoalesceGap) break;   // gap too wide → cut
            else ++j;
        }
        const uint32_t off = static_cast<uint32_t>(spanStart);
        const uint32_t len = static_cast<uint32_t>(spanEnd - spanStart);
        appendU32(out, off);
        appendU32(out, len);
        const size_t hdr = out.size();
        out.resize(hdr + len);
        for (uint32_t k = 0; k < len; ++k)
            out[hdr + k] = static_cast<uint8_t>(a[off + k] ^ b[off + k]);
        i = spanEnd;
    }
}

// Apply `delta` to `blob` in place. Returns false on a malformed/oversized
// record (defensive — RewindBuffer's own deltas are always well-formed).
bool applyXorDelta(std::vector<uint8_t>& blob, const std::vector<uint8_t>& delta)
{
    const size_t dn = delta.size();
    size_t p = 0;
    while (p + 8 <= dn) {
        const uint32_t off = readU32(delta, p); p += 4;
        const uint32_t len = readU32(delta, p); p += 4;
        if (p + len > dn) return false;
        if (static_cast<size_t>(off) + len > blob.size()) return false;
        for (uint32_t k = 0; k < len; ++k)
            blob[off + k] ^= delta[p + k];
        p += len;
    }
    return p == dn;
}

}  // namespace

RewindBuffer::RewindBuffer(size_t maxFrames)
    : maxFrames_(maxFrames ? maxFrames : 1)
{}

void RewindBuffer::setEnabled(bool on)
{
    const bool was = enabled_.exchange(on);
    // Re-enabling starts a new timeline. Force the next capture to be a
    // KEYFRAME rather than a delta against a base from before the pause —
    // see the header for what that splice did to the timeline.
    if (on && !was) {
        sinceKeyframe_ = 0;
        prevBlob_.clear();
    }
}

void RewindBuffer::setMaxFrames(size_t n)
{
    maxFrames_ = n ? n : 1;
    evictToCap();
}

void RewindBuffer::setMaxBytes(size_t n)
{
    maxBytes_ = n;
    evictToCap();
}

void RewindBuffer::clear()
{
    frames_.clear();
    totalBytes_ = 0;
    sinceKeyframe_ = 0;
    prevBlob_.clear();
}

void RewindBuffer::capture(M6502& cpu, Memory& mem)
{
    if (!enabled_.load()) return;

    // Time must move forward across the ring. It does not always move forward
    // in the machine: a scrub that resumes through anything but
    // `rewindEndAndResume` (the toolbar Play button, Machine > Run, the
    // `machine.run` palette command, the kiosk menu) leaves the abandoned
    // future in the deque, and the frames captured from the rewound point
    // then carry stamps EARLIER than the tail. Nothing crashes; the timeline
    // simply starts lying — `indexForCycle` stops at the first frame past its
    // target, so a seek lands far from the cycle asked for, and the "span"
    // readout (newest - oldest) goes wrong or negative.
    //
    // Drop that future here rather than at each resume site: this is the one
    // funnel every capture goes through, so the invariant holds for callers
    // that do not exist yet, and for a snapshot load that forgot to clear.
    dropAbandonedFuture(mem.getCycleCounter());

    // Serialize current state into captureScratch_ (the memory-backed writer
    // assigns the blob on scope exit).
    {
        SnapshotWriter w(captureScratch_);
        captureMachineState(w, cpu, mem, /*includeSlots=*/true);
    }

    Frame f;
    f.cycle = mem.getCycleCounter();

    // `sinceKeyframe_` is the previous frame's distance from its keyframe
    // (keyframe itself = 0). The frame we're about to store sits one further;
    // make it a keyframe once that distance reaches the interval, so
    // keyframes land at indices 0, interval, 2·interval, … A size change
    // (e.g. RamWorks bank count) also forces one — deltas need equal-length
    // neighbours.
    const bool sizeChanged = captureScratch_.size() != prevBlob_.size();
    if (frames_.empty() || sinceKeyframe_ + 1 >= keyframeInterval_ || sizeChanged) {
        f.isKeyframe = true;
        f.data = captureScratch_;
        sinceKeyframe_ = 0;
    } else {
        f.isKeyframe = false;
        encodeXorDelta(prevBlob_, captureScratch_, f.data);
        ++sinceKeyframe_;
    }

    totalBytes_ += f.data.size();
    frames_.push_back(std::move(f));
    prevBlob_ = std::move(captureScratch_);   // running full state for next delta
    evictToCap();
}

void RewindBuffer::evictToCap()
{
    // Evict while either cap is exceeded, but never below one frame (a lone
    // keyframe larger than the byte budget — e.g. a 10 MB RamWorks snapshot —
    // is still kept so there's always something to restore).
    auto overCap = [this]() {
        return frames_.size() > maxFrames_ ||
               (maxBytes_ != 0 && totalBytes_ > maxBytes_ && frames_.size() > 1);
    };
    while (overCap()) {
        // Invariant: the front is always a keyframe. Before dropping it, if
        // the next frame is a delta it must be promoted to a keyframe so the
        // chain doesn't dangle.
        if (frames_.size() >= 2 && !frames_[1].isKeyframe) {
            std::vector<uint8_t> full = frames_[0].data;   // front = keyframe full blob
            applyXorDelta(full, frames_[1].data);
            totalBytes_ -= frames_[1].data.size();
            frames_[1].data = std::move(full);
            frames_[1].isKeyframe = true;
            totalBytes_ += frames_[1].data.size();
        }
        totalBytes_ -= frames_[0].data.size();
        frames_.pop_front();
    }
}

void RewindBuffer::reconstruct(size_t index, std::vector<uint8_t>& out) const
{
    // Nearest keyframe at or below `index`. The front is always a keyframe,
    // so this terminates.
    size_t k = index;
    while (k > 0 && !frames_[k].isKeyframe) --k;
    out = frames_[k].data;
    for (size_t j = k + 1; j <= index; ++j)
        applyXorDelta(out, frames_[j].data);
}

void RewindBuffer::dropAbandonedFuture(uint64_t cycle)
{
    // Hot path: one compare per captured frame. The early-out is `<` and not
    // `<=` on purpose — a capture stamped exactly at the tail has not moved
    // forward either, and appending it would leave two frames the seek
    // helpers would have to tie-break.
    if (frames_.empty() || frames_.back().cycle < cycle) return;

    // Keep every frame strictly older than the incoming stamp. Walking from
    // the back is O(dropped), not O(size) — the common non-empty case here is
    // "the user scrubbed a few seconds back".
    size_t keep = frames_.size();
    while (keep > 0 && frames_[keep - 1].cycle >= cycle) --keep;
    if (keep == 0) {
        // Every retained frame is in the abandoned future (a full rewind to
        // before the oldest frame, or a snapshot load from another session).
        // clear() also resets the delta base, so the next capture is a
        // keyframe rather than a delta against a blob from that other future.
        clear();
        return;
    }
    truncateAfter(keep - 1);   // rebuilds prevBlob_ + sinceKeyframe_
}

void RewindBuffer::resyncSinceKeyframe()
{
    if (frames_.empty()) { sinceKeyframe_ = 0; return; }
    size_t k = frames_.size() - 1;
    while (k > 0 && !frames_[k].isKeyframe) --k;
    sinceKeyframe_ = (frames_.size() - 1) - k;
}

size_t RewindBuffer::keyframeCount() const
{
    size_t c = 0;
    for (const Frame& f : frames_) if (f.isKeyframe) ++c;
    return c;
}

RewindBuffer::FrameInfo RewindBuffer::infoAt(size_t index) const
{
    if (index >= frames_.size()) return {};
    return { frames_[index].cycle, frames_[index].data.size(), frames_[index].isKeyframe };
}

bool RewindBuffer::restore(size_t index, M6502& cpu, Memory& mem)
{
    if (index >= frames_.size()) return false;
    reconstruct(index, reconstructScratch_);
    SnapshotReader r(reconstructScratch_.data(), reconstructScratch_.size());
    if (!r.good()) return false;
    return restoreMachineState(r, cpu, mem, /*transactional=*/false).ok;
}

size_t RewindBuffer::indexForCycle(uint64_t cycle) const
{
    if (frames_.empty()) return kNoFrame;
    size_t idx = 0;
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].cycle <= cycle) idx = i;
        else break;
    }
    return idx;
}

size_t RewindBuffer::restoreToCycle(uint64_t cycle, M6502& cpu, Memory& mem)
{
    const size_t idx = indexForCycle(cycle);
    if (idx == kNoFrame) return kNoFrame;
    return restore(idx, cpu, mem) ? idx : kNoFrame;
}

void RewindBuffer::truncateAfter(size_t index)
{
    if (index >= frames_.size()) return;     // nothing after it
    while (frames_.size() > index + 1) {
        totalBytes_ -= frames_.back().data.size();
        frames_.pop_back();
    }
    // The newest frame changed: rebuild prevBlob_ (the delta base) and the
    // keyframe-spacing counter so the next capture continues correctly.
    reconstruct(frames_.size() - 1, prevBlob_);
    resyncSinceKeyframe();
}

}  // namespace pom2
