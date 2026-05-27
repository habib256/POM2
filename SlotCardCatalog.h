// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlotCardCatalog — the single list of card types the user can assign to an
// expansion slot, plus the ROM-presence probes that gate the conditional
// entries (Mouse, CFFA). Extracted from MainWindow_Slots.cpp so BOTH the
// legacy Slot Configuration panel and the consolidated Slot Manager panel
// drive their dropdowns and built-in-name resolution from one source.

#ifndef POM2_SLOT_CARD_CATALOG_H
#define POM2_SLOT_CARD_CATALOG_H

#include <filesystem>
#include <string>
#include <string_view>

namespace pom2 {

struct CardType {
    const char* key;
    const char* label;
};

// Card types the user can pick for any slot. Index 0 is the empty slot.
inline constexpr CardType kCardTypes[] = {
    { "",             "(empty)"           },
    { "diskii",       "Disk II"           },
    { "hdv",          "ProDOS HDV"        },
    // CFFA 2.0 — MAME-faithful IDE/CompactFlash card: real firmware over an
    // emulated ATA chip (vs the synthetic "hdv"). Needs roms/cffa20ee02.bin
    // (6502) or cffa20eec02.bin (65C02); hidden from the picker when absent.
    { "cffa",         "CFFA 2.0 (IDE)"    },
    // SmartPort 3.5" — Apple Disk 3.5 Controller card (the "Liron" /
    // 670-0186). Brings 2× ProDOS block units (3.5" 800K or HDV) to a //e
    // or II+ via the standard ProDOS block-device protocol, no IWM.
    { "smartport35",  "SmartPort 3.5\""   },
    { "ssc",          "Super Serial"      },
    // Printer (parallel) — synthetic card that spools COUT bytes to a host
    // file (.txt / .pdf). Built-in at slot 1 of //c / //c+, free-slot pick
    // on II / II+ / //e. No PROM dump needed.
    { "printer",      "Printer (Parallel)" },
    { "clock",        "Clock (ProDOS)"    },
    { "chatmauve",    "Le Chat Mauve"     },
    { "mouse",        "Mouse Interface"   },
    // AppleWin-style HLE variant — only needs the slot EPROM (no MCU mask
    // ROM). Different code path from "mouse" (no MC68705 emulation).
    { "mouseaw",      "Mouse (AppleWin HLE)" },
    { "mockingboard", "Mockingboard A/C"  },
    // Phasor (Applied Engineering) — dual-mode successor to Mockingboard.
    // Starts in MB-compat mode (2 active AYs), software-switchable to
    // native (4 AYs, 12 voices, doubled chip clock). Audio synth = TODO.
    { "phasor",       "Phasor (AE)"       },
    // Echo+ (Street Electronics) — standalone SSI263 speech synth at
    // $Cs00-$Cs04. Pairs naturally with a Mockingboard A/C at another
    // slot. v1: register state machine + IRQ timing complete, phoneme
    // PCM data deferred to a separate commit (LGPL question).
    { "echoplus",     "Echo+ (SSI263)"    },
};

/// Human-readable label for a card key (falls back to the key itself).
inline const char* cardLabelForKey(std::string_view key)
{
    for (const auto& ct : kCardTypes)
        if (key == ct.key) return ct.label;
    // Caller passed a key not in the catalog — return something printable.
    static thread_local std::string scratch;
    scratch.assign(key);
    return scratch.c_str();
}

inline bool mouseRomsPresent()
{
    namespace fs = std::filesystem;
    bool slotRom = false, mcuRom = false;
    for (const char* p : { "roms/mouse_341-0270-c.bin",
                           "../roms/mouse_341-0270-c.bin",
                           "../../roms/mouse_341-0270-c.bin" }) {
        if (fs::exists(p)) { slotRom = true; break; }
    }
    for (const char* p : { "roms/mouse_341-0269.bin",
                           "../roms/mouse_341-0269.bin",
                           "../../roms/mouse_341-0269.bin" }) {
        if (fs::exists(p)) { mcuRom = true; break; }
    }
    return slotRom && mcuRom;
}

/// AppleWin-style Mouse HLE needs only the 2 KB slot EPROM (the MCU side
/// is synthesised in C++). Reuses the same `mouse_341-0270-c.bin` dump.
inline bool mouseAwRomPresent()
{
    namespace fs = std::filesystem;
    for (const char* p : { "roms/mouse_341-0270-c.bin",
                           "../roms/mouse_341-0270-c.bin",
                           "../../roms/mouse_341-0270-c.bin" }) {
        if (fs::exists(p)) return true;
    }
    return false;
}

inline bool cffaRomPresent()
{
    namespace fs = std::filesystem;
    for (const char* n : { "cffa20ee02.bin", "cffa20eec02.bin" })
        for (const char* dir : { "roms/", "../roms/", "../../roms/" }) {
            if (fs::exists(std::string(dir) + n)) return true;
        }
    return false;
}

} // namespace pom2

#endif // POM2_SLOT_CARD_CATALOG_H
