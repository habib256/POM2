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
    // Grappler+ (Orange Micro) — parallel printer card with a 4 KB ROM
    // providing graphics dump commands (^I G / ^I H). Requires a ROM
    // dump in roms/grappler_plus.bin; falls back to a stub when missing.
    // See markadev/AppleII-RevEng/Orange-Micro-Grappler+.
    { "grappler",     "Grappler+ (Orange Micro)" },
    { "clock",        "Clock (ProDOS)"    },
    // Uthernet I (a2RetroSystems) — CS8900A Ethernet NIC. Raw frames
    // only: the Apple-side stack (IP65, Contiki, ADTPro-ethernet) does
    // the TCP/IP. Needs a host Ethernet transport to see traffic, which
    // in practice means a libslirp-enabled build; the card still plugs
    // and probes without one. MAME `bus/a2bus/uthernet.cpp` port.
    { "uthernet",     "Uthernet I (CS8900A)" },
    // Uthernet II (a2RetroSystems) — WIZnet W5100 hardware TCP/IP stack.
    // Its four TCP/UDP sockets map onto host BSD sockets, so IRC / telnet
    // / FTP clients work with no libslirp and no privileges; only its
    // MACRAW / IPRAW modes need a backend. No MAME device — ported from
    // AppleWin `source/Uthernet2.cpp`.
    { "uthernet2",    "Uthernet II (W5100)" },
    // FujiNet (fujinet.online) — SmartPort controller that RELAYS every call
    // to a real FujiNet over the project's SP-over-SLIP protocol: a FujiNet
    // desktop build on loopback TCP, or a physical ESP32 board over USB
    // CDC-ACM. One card carries every FujiNet function (block storage, the
    // N: network device, clock, printer, modem, CP/M) because on the Apple II
    // they are all SmartPort units. No ROM dump needed — POM2 synthesises the
    // slot ROM. Slot 7 by convention so the autostart scan reaches it before
    // the Disk II in slot 6. II+ / //e only: a //c-class machine's forced
    // INTCXROM masks slot ROM entirely.
    { "fujinet",      "FujiNet (SP over SLIP)" },
    // Microsoft SoftCard — Z80 DMA bus-master card for CP/M. No ROM
    // needed (the card has none; the CP/M boot disk finds it by toggling
    // slot ROM windows). MAME a2bus/a2softcard.cpp port.
    { "softcard",     "SoftCard Z80 (CP/M)" },
    { "chatmauve",    "Le Chat Mauve"     },
    { "mouse",        "Mouse Interface"   },
    // AppleWin-style HLE variant — only needs the slot EPROM (no MCU mask
    // ROM). Different code path from "mouse" (no MC68705 emulation).
    { "mouseaw",      "Mouse (AppleWin HLE)" },
    { "mockingboard", "Mockingboard A/C"  },
    // Mockingboard "C" Sound II — A/C base + SSI263A speech synth at
    // $C(s)40-$C(s)44. Drives speech in Ultima IV/V, Wasteland, Bard's
    // Tale, Crime Wave, Hudson Hawk, etc. (any title that targets the
    // Sound II variant). A/!R wired to VIA1.CA1 → IRQ-driven phoneme
    // dequeue. Phoneme PCM ported from AppleWin (GPL3 compat).
    { "mockingboard_c", "Mockingboard C (Sound II)" },
    // Phasor (Applied Engineering) — dual-mode successor to Mockingboard.
    // Starts in MB-compat mode (2 active AYs), software-switchable to
    // native (4 AYs, 12 voices, doubled chip clock). Audio synth = TODO.
    { "phasor",       "Phasor (AE)"       },
    // Cricket / Echo-class SSI263 card — standalone SSI263 speech synth
    // at $Cs00-$Cs04. Historically shipped under the "Echo+" label in
    // POM2 settings (key stays "echoplus" for back-compat) but markadev's
    // dumps confirm Street Electronics' actual Echo+ used 2× AY-3-8913
    // + TMS5220, not the SSI263. The SSI263-based product line was the
    // Cricket. Pairs with a Mockingboard A/C in another slot.
    { "echoplus",     "Cricket / Echo (SSI263)" },
    // Echo+ (real) — Street Electronics ECHO+ as actually shipped:
    // 2× AY-3-8913 PSGs + TMS5220 speech chip. Scaffolded stub — chip
    // models silent in v1; the register decode is enough for software
    // detection. See markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+.
    { "echoplus_tms", "Echo+ (TMS5220 + 2×AY) — silent, detect-only" },
    // Apple II Workstation Card — LocalTalk/AppleTalk, and a coprocessor:
    // its own 65C02 + 8530 SCC run inside the card. ROM-gated on the 64 KiB
    // 341-0358-A dump; the host handshake at $C0nX is not yet established,
    // so the guest's AppleTalk stack will not complete a transaction.
    { "workstation",  "Apple II Workstation Card (LocalTalk) — boots, host link WIP" },
    // 4play (Lukazi, 2016) — four DIGITAL joysticks, one byte each at
    // $C0nX. The Apple game port is analogue and carries two paddles; this
    // is how an Apple II gets four players.
    { "4play",        "4play — 4 digital joysticks (Lukazi)" },
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
