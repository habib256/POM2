// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "AtaBlockDevice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pom2 {

namespace {
bool ataTraceOn()
{
    static const bool on = std::getenv("POM2_TRACE_CFFA") != nullptr;
    return on;
}
} // namespace

void AtaBlockDevice::reset()
{
    error_       = 0x00;
    features_    = 0x00;
    sectorCount_ = 0x00;
    lba0_ = lba1_ = lba2_ = 0x00;
    devHead_     = 0xA0;
    status_      = kStDRDY | kStDSC;
    control_     = 0x00;
    phase_       = Phase::Idle;
    lba_         = 0;
    sectorsLeft_ = 0;
    wordIdx_     = 0;
    wordBuf_.fill(0);
}

uint32_t AtaBlockDevice::currentLba() const
{
    return (static_cast<uint32_t>(devHead_ & 0x0F) << 24) |
           (static_cast<uint32_t>(lba2_) << 16) |
           (static_cast<uint32_t>(lba1_) << 8) |
            static_cast<uint32_t>(lba0_);
}

void AtaBlockDevice::loadSectorToBuffer()
{
    uint8_t sector[Block512Backing::kBlockBytes];
    if (!backing_.readBlock(lba_, sector)) {
        std::memset(sector, 0, sizeof(sector)); // out-of-range LBA reads as zeros
    }
    for (size_t i = 0; i < 256; ++i) {
        wordBuf_[i] = static_cast<uint16_t>(sector[2 * i]) |
                      (static_cast<uint16_t>(sector[2 * i + 1]) << 8);
    }
    wordIdx_ = 0;
}

void AtaBlockDevice::flushBufferToSector()
{
    uint8_t sector[Block512Backing::kBlockBytes];
    for (size_t i = 0; i < 256; ++i) {
        sector[2 * i]     = static_cast<uint8_t>(wordBuf_[i] & 0xFF);
        sector[2 * i + 1] = static_cast<uint8_t>(wordBuf_[i] >> 8);
    }
    backing_.writeBlock(lba_, sector); // false (WP / OOR) is silently dropped
    wordIdx_ = 0;
}

void AtaBlockDevice::fillIdentify()
{
    wordBuf_.fill(0);
    const uint32_t total = static_cast<uint32_t>(backing_.blockCount());

    // Plausible CHS geometry for legacy callers (16 heads × 63 sectors).
    const uint32_t spt   = 63;
    const uint32_t heads = 16;
    uint32_t cyls = total / (spt * heads);
    if (cyls > 0xFFFF) cyls = 0xFFFF;

    auto putString = [&](size_t firstWord, size_t wordLen, const char* s) {
        // ATA strings are space-padded and byte-swapped within each word.
        for (size_t w = 0; w < wordLen; ++w) {
            char hi = *s ? *s++ : ' ';
            char lo = *s ? *s++ : ' ';
            wordBuf_[firstWord + w] =
                static_cast<uint16_t>((static_cast<uint8_t>(hi) << 8) |
                                       static_cast<uint8_t>(lo));
        }
    };

    wordBuf_[0]  = 0x0040;                 // general config: fixed device
    wordBuf_[1]  = static_cast<uint16_t>(cyls);
    wordBuf_[3]  = static_cast<uint16_t>(heads);
    wordBuf_[6]  = static_cast<uint16_t>(spt);
    putString(10, 10, "POM2-0001");        // serial number  (words 10..19)
    putString(23, 4,  "1.0");              // firmware rev   (words 23..26)
    putString(27, 20, "POM2 ATA Block Device"); // model      (words 27..46)
    wordBuf_[47] = 0x8001;                 // max sectors per READ/WRITE MULTIPLE = 1
    wordBuf_[49] = 0x0200;                 // capabilities: LBA supported (bit 9)
    wordBuf_[53] = 0x0001;                 // words 54-58 (current CHS/capacity) valid
    // Current capacity in sectors (words 57-58, 32-bit low-word-first). The
    // CFFA firmware reads THIS field — not 60-61 — to size its partitions
    // (a2cffa firmware $CD35-$CD52); leaving it zero ⇒ 0 partitions ⇒ Err $28.
    wordBuf_[57] = static_cast<uint16_t>(total & 0xFFFF);
    wordBuf_[58] = static_cast<uint16_t>((total >> 16) & 0xFFFF);
    wordBuf_[60] = static_cast<uint16_t>(total & 0xFFFF);        // LBA28 total,
    wordBuf_[61] = static_cast<uint16_t>((total >> 16) & 0xFFFF);// low word first

    wordIdx_ = 0;
}

void AtaBlockDevice::startCommand(uint8_t cmd)
{
    error_  = 0x00;
    status_ = kStDRDY | kStDSC; // BSY would pulse on real silicon; we settle instantly

    if (ataTraceOn()) {
        std::fprintf(stderr,
            "[ATA] cmd=$%02X lba=%u count=%u devHead=$%02X blocks=%zu\n",
            cmd, currentLba(),
            (sectorCount_ == 0) ? 256u : sectorCount_,
            devHead_, backing_.blockCount());
    }

    switch (cmd) {
        case kCmdIdentify:
            fillIdentify();
            sectorsLeft_ = 1;
            phase_  = Phase::PioIn;
            status_ = kStDRDY | kStDSC | kStDRQ;
            break;

        case kCmdRead:
        case kCmdReadMulti: {
            lba_         = currentLba();
            sectorsLeft_ = (sectorCount_ == 0) ? 256 : sectorCount_;
            loadSectorToBuffer();
            phase_  = Phase::PioIn;
            status_ = kStDRDY | kStDSC | kStDRQ;
            break;
        }

        case kCmdWrite:
        case kCmdWriteMulti: {
            // ATA-1: a WRITE to a write-protected device must abort — ERR set
            // in Status, ABRT in the Error register, no DRQ — rather than
            // accept the data and silently drop it in flushBufferToSector
            // (which returns "success" to ProDOS). The HDV synthetic card
            // surfaces WP via its own status path; the ATA/CFFA path does it
            // here so a WP 2IMG can't be "written" without an error.
            if (backing_.isWriteProtected()) {
                error_  = kErrABRT;
                status_ = kStDRDY | kStDSC | kStDF | kStERR;
                phase_  = Phase::Idle;
                break;
            }
            lba_         = currentLba();
            sectorsLeft_ = (sectorCount_ == 0) ? 256 : sectorCount_;
            wordIdx_     = 0;
            phase_  = Phase::PioOut;
            status_ = kStDRDY | kStDSC | kStDRQ; // host now feeds the first sector
            break;
        }

        default:
            // Init-params / set-features / flush / standby / etc. — accept and
            // complete with no data phase.
            phase_  = Phase::Idle;
            status_ = kStDRDY | kStDSC;
            break;
    }
}

uint16_t AtaBlockDevice::cs0_r(uint8_t reg)
{
    switch (reg & 0x07) {
        case 0: { // data port (PIO in)
            if (phase_ != Phase::PioIn) return 0xFFFF;
            const uint16_t w = wordBuf_[wordIdx_++];
            if (wordIdx_ >= 256) {
                // Sector complete.
                if (sectorsLeft_ > 0) --sectorsLeft_;
                if (sectorsLeft_ > 0) {
                    ++lba_;
                    loadSectorToBuffer();      // DRQ stays set for the next sector
                } else {
                    phase_  = Phase::Idle;
                    status_ = kStDRDY | kStDSC; // DRQ cleared
                }
            }
            return w;
        }
        case 1: return error_;
        case 2: return sectorCount_;
        case 3: return lba0_;
        case 4: return lba1_;
        case 5: return lba2_;
        case 6: return devHead_;
        case 7: return status_;
    }
    return 0xFF;
}

void AtaBlockDevice::cs0_w(uint8_t reg, uint16_t val)
{
    const uint8_t v = static_cast<uint8_t>(val & 0xFF);
    switch (reg & 0x07) {
        case 0: { // data port (PIO out)
            if (phase_ != Phase::PioOut) return;
            wordBuf_[wordIdx_++] = val;
            if (wordIdx_ >= 256) {
                flushBufferToSector();
                if (sectorsLeft_ > 0) --sectorsLeft_;
                if (sectorsLeft_ > 0) {
                    ++lba_;                    // DRQ stays set for the next sector
                } else {
                    phase_  = Phase::Idle;
                    status_ = kStDRDY | kStDSC; // DRQ cleared
                }
            }
            return;
        }
        case 1: features_    = v; return;
        case 2: sectorCount_ = v; return;
        case 3: lba0_        = v; return;
        case 4: lba1_        = v; return;
        case 5: lba2_        = v; return;
        case 6: devHead_     = v; return;
        case 7: startCommand(v);  return;
    }
}

uint16_t AtaBlockDevice::cs1_r(uint8_t reg)
{
    // Offset 6 = alternate status (same value as the primary status register,
    // but reading it does not clear a pending interrupt — moot, no IRQ here).
    if ((reg & 0x07) == 6) return status_;
    return 0xFF;
}

void AtaBlockDevice::cs1_w(uint8_t reg, uint16_t val)
{
    // Offset 6 = device control: bit 2 = SRST (software reset).
    if ((reg & 0x07) == 6) {
        control_ = static_cast<uint8_t>(val & 0xFF);
        if (control_ & 0x04) reset();
    }
}

} // namespace pom2
