// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Ssi263PhonemeData — declarations for the 62-phoneme PCM blob used
// by the SSI263 speech synth. Data lives in `Ssi263PhonemeData.cpp`
// (313 KB of int16 PCM, ported from AppleWin under LGPL → GPL3
// compat); this header just exposes the lookup table + the data
// pointer so `Ssi263::fillAudio` can pull samples without dragging
// the full literal array through every consumer's preprocessor pass.

#ifndef POM2_SSI263_PHONEME_DATA_H
#define POM2_SSI263_PHONEME_DATA_H

#include <cstddef>
#include <cstdint>

namespace pom2::ssi263_data {

struct PhonemeInfo {
    uint32_t offset;     // byte offset into kPhonemeData (uint16_t units)
    uint32_t length;     // sample count in uint16_t units
};

inline constexpr size_t kNumPhonemes    = 62;
inline constexpr size_t kPhonemeDataLen = 156566;
/// Native sample rate of the SSI263 phoneme blob (AppleWin convention).
/// The Ssi263 synth resamples to the host audio rate.
inline constexpr uint32_t kPhonemeSampleRateHz = 22050;

extern const PhonemeInfo kPhonemeInfo[kNumPhonemes];
extern const uint16_t    kPhonemeData[kPhonemeDataLen];

} // namespace pom2::ssi263_data

#endif // POM2_SSI263_PHONEME_DATA_H
