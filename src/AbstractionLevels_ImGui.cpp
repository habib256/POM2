// VERHILLE Arnaud 2026
//
// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AbstractionLevels_ImGui — implementation. See the header for why this
// window exists; `docs/lle_vs_hle.md` is the reference for every level in
// the catalog below, and the two must be edited together.

#include "AbstractionLevels_ImGui.h"

#include "Pom2Theme.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cstring>
#include <unordered_map>

namespace pom2 {

// ─── The scale ───────────────────────────────────────────────────────────

const char* levelBadge(AbsLevel l)
{
    switch (l) {
        case AbsLevel::L0:   return "L0";
        case AbsLevel::L1:   return "L1";
        case AbsLevel::L2:   return "L2";
        case AbsLevel::H1:   return "H1";
        case AbsLevel::H2:   return "H2";
        case AbsLevel::Host: return "—";
    }
    return "?";
}

const char* levelName(AbsLevel l)
{
    switch (l) {
        case AbsLevel::L0:   return "Silicon";
        case AbsLevel::L1:   return "Chip-faithful";
        case AbsLevel::L2:   return "Real firmware, host device";
        case AbsLevel::H1:   return "Synthetic firmware";
        case AbsLevel::H2:   return "Host function";
        case AbsLevel::Host: return "Host-side machinery";
    }
    return "?";
}

bool levelIsLle(AbsLevel l)
{
    return l == AbsLevel::L0 || l == AbsLevel::L1 || l == AbsLevel::L2;
}

namespace {

// Colour per level. Deliberately NOT a good/bad ramp: H1 is the right answer
// for a card with no public ROM dump, and painting it red would be a lie
// about POM2's own decision rule. Green→blue→cyan walks *down* the stack,
// amber marks "the boundary is the service", grey means off-axis.
ImU32 levelColour(AbsLevel l)
{
    const Palette& p = palette();
    switch (l) {
        case AbsLevel::L0:   return p.ok;
        case AbsLevel::L1:   return p.info;
        case AbsLevel::L2:   return p.accent;
        case AbsLevel::H1:   return p.warn;
        case AbsLevel::H2:   return p.textDim;
        case AbsLevel::Host: return p.textDim;
    }
    return p.text;
}

// A filled chip rather than coloured text: the level is the column the eye
// scans, and at a glance a chip separates from prose in a way a coloured
// word does not. Drawn by hand because ImGui has no badge primitive.
void levelChip(AbsLevel l)
{
    const ImU32 col = levelColour(l);
    const char* txt = levelBadge(l);
    const ImVec2 sz = ImGui::CalcTextSize(txt);
    const ImVec2 pad(ImGui::GetStyle().FramePadding.x, 1.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + sz.x + pad.x * 2.0f, p0.y + sz.y + pad.y * 2.0f);

    ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(f.x, f.y, f.z, 0.18f)),
                      3.0f);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(
                            ImVec4(f.x, f.y, f.z, 0.55f)),
                3.0f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), col, txt);
    ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

} // namespace

// ─── The catalog ─────────────────────────────────────────────────────────
//
// Mirrors the master table of `docs/lle_vs_hle.md`, in the same order and
// with the same verdicts. The doc carries the evidence (file:line citations,
// MAME references); these one-liners carry only the conclusion, because a
// table cell the user has to scroll is a table cell they will not read.

const std::vector<AbsEntry>& abstractionCatalog()
{
    static const std::vector<AbsEntry> kCatalog = {
    // ── CPU & memory ────────────────────────────────────────────────────
    { "cpu", "CPU & memory", "6502 / 65C02 / Rockwell / WDC", AbsLevel::L0,
      "Per-cycle; 100% of the 178 documented NMOS opcodes vs Tom Harte 65x02; "
      "WDC decimal SBC silicon-exact including interdigit carry",
      "This is the floor.", "M6502.h/.cpp" },
    { "z80", "CPU & memory", "Z80 core", AbsLevel::L0,
      "zexdoc + zexall clean; MEMPTR and X/Y flags modelled, not approximated",
      "This is the floor.", "Z80.h/.cpp" },
    { "softcard", "CPU & memory", "SoftCard Z80 (DMA bus master)", AbsLevel::L1,
      "Real DMA arbitration, 6502 halted per instruction slice; CP/M 2.2 boots "
      "identical to the MAME oracle",
      "Nothing above the arbitration is guest-visible.", "SoftCardZ80.h/.cpp" },
    { "memory", "CPU & memory", "Memory / MMU / IOU / RamWorks", AbsLevel::L1,
      "Soft switches, aux paging, LC banks, power-on 00 FF pattern, per-cycle "
      "floating bus (vapor lock)",
      "Behaviour is already at L1; only the god-object split is pending.",
      "Memory.h/.cpp" },

    // ── Video ───────────────────────────────────────────────────────────
    { "display", "Video", "Display (beam-raced reconstruction)", AbsLevel::L1,
      "Per-byte column reconstruction from a cycle-stamped video-event log; "
      "mid-scanline mode splits at 280/560 px",
      "A true per-scanline incremental renderer (MAME style) is the remaining "
      "L0 step — it would fix the unidirectional mid-frame page-split limit.",
      "Apple2Display.h/.cpp" },
    { "oe", "Video", "Composite NTSC (OpenEmulator)", AbsLevel::L1,
      "14.318 MHz 1-bit signal → FIR demodulation (Y @ 2.0, C @ 0.6 MHz) → "
      "YUV→RGB, PAL line-phase alternation",
      "Pure-analog IIR-on-signal is deferred as academic (5-10 d).",
      "NtscPostProcessor.*, OpenGLShader.*" },
    { "lut", "Video", "Artifact-colour LUT modes", AbsLevel::H1,
      "MAME's composite colour tables indexed per dot pattern — the RESULT of "
      "NTSC artifacting, tabulated, with no signal in between",
      "Not a defect: it is the cheap path, and the OE pipeline beside it is "
      "the low-level one. Switchable above.",
      "Apple2Display.cpp, AppleWinNtsc.h/.cpp" },
    { "chatmauve", "Video", "Le Chat Mauve RGB", AbsLevel::L1,
      "AN3 pulse FIFO decode (real register state machine) + AppleWin "
      "RGBMonitor pixel rules; Eve colour text and HGR Duochrome decoded",
      "Nothing below the register interface is guest-visible.",
      "LeChatMauveCard.h/.cpp" },

    // ── Audio ───────────────────────────────────────────────────────────
    { "speaker", "Audio", "Speaker", AbsLevel::L0,
      "Verbatim MAME spkrdev.cpp: 4x oversample, 64-tap windowed sinc, "
      "0.995-pole DC blocker",
      "This is the floor.", "SpeakerDevice.*" },
    { "cassette", "Audio", "Cassette", AbsLevel::L1,
      "Real $C020 flip-flop and $C060 comparator sign; the guest's own Monitor "
      "loops time real zero-crossings out of a host WAV",
      "The tape itself is analog and lives on the host.", "CassetteDevice.*" },
    { "mockingboard", "Audio", "Mockingboard / Phasor (6522 + AY-3-8910)",
      AbsLevel::L1,
      "T1/T2, IFR/IER, port latches + DDR, CA1 edges; AY counters, LFSR and "
      "envelope generator",
      "Documented skips (shift register, CA2/CB1/CB2 handshake, PB6 pulse "
      "counting) are wired by no POM2 card.",
      "Mockingboard.*, Via6522.h, Ay3_8910.h" },
    { "ssi263", "Audio", "SSI263 speech", AbsLevel::H1,
      "Registers, A/!R handshake, IRQ modes and phoneme DURATION are "
      "chip-exact (L1); the sound itself is a canned PCM blob per phoneme",
      "The real chip is an analog formant synth. AppleWin's blob is the only "
      "extant reference — MAME has no SSI263 at all.",
      "Ssi263.*, Ssi263PhonemeData.*" },
    { "tms5220", "Audio", "Echo+ TMS5220 (scaffold)", AbsLevel::H1,
      "Stub register decode at $Cs00-$Cs0F, enough for driver detection",
      "The LPC10 decoder (chirp ROM + K-parameter interpolation) and the "
      "AY-3-8913 synth are not written yet — ~3-5 d.",
      "EchoPlusTMS5220Card.*" },
    { "floppysnd", "Audio", "Floppy mechanical sounds", AbsLevel::H2,
      "Host sample playback driven by emuCycles-stamped phase strobes",
      "There is nothing on the bus to model — it is literally acoustics.",
      "FloppySoundDevice.*" },

    // ── Storage ─────────────────────────────────────────────────────────
    { "diskimage", "Storage", "DiskImage / WOZ", AbsLevel::L0,
      "Bit-cell / flux-transition store; getNextTransition is verbatim MAME "
      "floppy.cpp",
      "A .dsk has no flux, so its bitstream is RECONSTRUCTED (sync-FF padding "
      ">= 5) — exactly what real hardware infers.",
      "DiskImage.*" },
    { "diskii", "Storage", "Disk II controller", AbsLevel::L0,
      "Real 341-0028-A P6 LSS PROM indexed per LSS cycle, real P5A boot PROM, "
      "per-drive angular position",
      "The legacy 32-cycle nibble gate is the H1 fallback, used only when "
      "roms/diskii_p6.rom is absent.",
      "DiskIICard.*" },
    { "iwm", "Storage", "IWM", AbsLevel::L0,
      "Verbatim MAME machine/iwm.cpp: m_active / m_rw / read-walker / "
      "write-window state machines",
      "Only the sub-CPU-cycle Q3 phase is unmodelled.", "IWMDevice.*" },
    { "sony35", "Storage", "SmartPort hub / Sony 3.5\" drive", AbsLevel::L0,
      "Zoned GCR, LSTRB register strobes, DSKCHG latch polarity per MAME "
      "floppy.cpp",
      "Nothing above it is abstracted.", "SmartPortHub.*, Sony35Drive.*" },
    { "cffa", "Storage", "CFFA 2.0 (IDE)", AbsLevel::L2,
      "The real 4 KB firmware dump EXECUTES over an ATA taskfile model "
      "isomorphic to MAME's cs0_r/cs0_w",
      "The ATA layer skips DMA / IRQ / SMART; CHD backing is phase 2.",
      "CffaCard.*, AtaBlockDevice.*" },
    { "hdv", "Storage", "ProDOS HDV card", AbsLevel::H1,
      "Hand-assembled 256 B slot ROM + an invented 4-register streaming port; "
      "block moves are a host memcpy. No GCR, no flux, no ATA",
      "Deliberate: it mounts .hdv/.2mg directly with NO card ROM dump "
      "required. H1 is the feature.",
      "ProDOSHardDiskCard.*" },
    { "smartportcard", "Storage", "SmartPort card (Liron-class)", AbsLevel::L2,
      "The whole 256 B slot page and the 2 KB $C800 bank come from the real "
      "roms/liron.rom; only the service entries are overlaid onto POM2's own "
      "SmartPort handler",
      "Full Liron LLE needs the IWM bit-shifter AND the UniDisk drive-side "
      "65C02 — out of scope.",
      "SmartPortCard.*" },
    { "iicsp", "Storage", "//c-class on-board SmartPort", AbsLevel::H1,
      "A $C500-$C5FF hole punched through the //c's forced INTCXROM, armed "
      "only by an explicit GUI/CLI boot",
      "A real //c masks all slot ROM, and MAME models no 3.5\" on a plain //c. "
      "Half an LLE hangs; a complete HLE boots.",
      "Memory.cpp, SmartPortHub.*" },
    { "prodosvol", "Storage", "ProDOS host folder", AbsLevel::H1,
      "POM2 FABRICATES a valid ProDOS volume image once; from then on the "
      "guest does genuine block reads through the real filesystem code",
      "The fabrication IS the abstraction — nothing below it is faked.",
      "ProDOSVolume.*" },

    // ── Network & serial ────────────────────────────────────────────────
    { "ssc", "Network & serial", "Super Serial Card", AbsLevel::H1,
      "6551 ACIA is register-faithful (L1); the slot ROM is synthetic — PR#n / "
      "IN#n hooks plus a Pascal 1.1 ID block",
      "The chip is right; the firmware is a stub because no dump is bundled. "
      "The real 341-0065-A is publicly dumped — this is a sourcing job.",
      "SuperSerialCard.h/.cpp" },
    { "uthernet", "Network & serial", "Uthernet I (CS8900A)", AbsLevel::L1,
      "Verbatim MAME machine/cs8900a.cpp (VICE lineage), packet-level",
      "RX is pull-mode: POM2 has no device_network_interface push bus.",
      "UthernetCard.*, Cs8900aDevice.*" },
    { "uthernet2", "Network & serial", "Uthernet II (W5100)", AbsLevel::L1,
      "Register/socket model per AppleWin + the WIZnet datasheet; each W5100 "
      "socket owns a real host BSD socket",
      "The chip IS a TCP/IP offload engine — host sockets are the faithful "
      "model, not a shortcut. Looks like HLE; is not.",
      "UthernetIICard.*, W5100Device.*" },
    { "fujinet", "Network & serial", "FujiNet relay", AbsLevel::H1,
      "Synthetic 256 B slot ROM whose only job is to trap into the host; every "
      "SmartPort call is forwarded verbatim over SP-over-SLIP",
      "Nothing below the protocol exists to model — the device is real and "
      "off-box.",
      "FujiNetCard.*, SpOverSlipLink.*" },
    { "netbackend", "Network & serial", "Ethernet host transport", AbsLevel::H2,
      "Null / Loopback / libslirp user-mode NAT",
      "Outbound-only by design: no root, no TAP, no pcap.",
      "NetworkBackend.h, SlirpNetworkBackend.*" },

    // ── Printing ────────────────────────────────────────────────────────
    { "printercard", "Printing", "Printer card (parallel)", AbsLevel::H1,
      "Synthetic ROM whose entire job is the PR#n CSWL/CSWH hook plus a 4-byte "
      "trampoline; the data port spools to a std::vector",
      "No PROM dump exists to run. The Pascal entry block is deliberately "
      "absent, so Pascal drivers cannot bind — BASIC PR#n only.",
      "PrinterCard.h/.cpp" },
    { "grappler", "Printing", "Grappler+ (Orange Micro)", AbsLevel::L2,
      "The real 4 KB Orange Micro EPROM EXECUTES; status byte, register "
      "decode, $C800 banking and S1 DIPs line-cited against MAME grappler.cpp",
      "The /STROBE 7-clock pulse is collapsed to instant — the synthetic "
      "printer consumes at latch time, so no observer exists.",
      "GrapplerCard.h/.cpp" },
    { "imagewriter", "Printing", "ImageWriter II printer", AbsLevel::H2,
      "Host-side printer: full control language, 4-band ribbon, 8/24-pin bit "
      "images, PNG/PDF export. Not a bus device at all",
      "There is no Apple II hardware here — the printer sat on the far side "
      "of a cable.",
      "ImageWriter.*, ImageWriterPdf.*" },

    // ── Input & clocks ──────────────────────────────────────────────────
    { "mouse", "Input & clocks", "Mouse Card — MAME", AbsLevel::L0,
      "An M68705P3 MCU EXECUTING its real 2 KB mask ROM at 2x CPU clock, plus "
      "an MC6821 PIA and quadrature edge generation",
      "Only the PAL16R4 chip-select sequencer is skipped (firmware-invisible).",
      "MouseCard.*" },
    { "mouseaw", "Input & clocks", "Mouse Card — AppleWin HLE", AbsLevel::H1,
      "Same slot EPROM, but the MCU is a C++ command-byte state machine; the "
      "position is copied from the host delta",
      "Ships BECAUSE the MCU mask ROM is not always available. Smoother and "
      "less correct: no quadrature rate limit.",
      "MouseCardAppleWin.*" },
    { "joystick", "Input & clocks", "Joystick / paddles", AbsLevel::L1,
      "Real $C070 RC discharge timing, sampled at $C064-$C067 bit 7",
      "Nothing below the timing is guest-visible.", "JoystickInput.h/.cpp" },
    { "clock", "Input & clocks", "ProDOS clock card (ThunderClock+)",
      AbsLevel::L2,
      "uPD1990AC bit-bang state machine per MAME upd1990a.cpp, driving the "
      "REAL Thunderware Rev 1.3 EPROM",
      "Already there. The dump even settled the 40-vs-48-bit shift-register "
      "question by disassembly.",
      "ClockCard.h/.cpp" },
    { "nsclock", "Input & clocks", "No-Slot Clock (DS1216E)", AbsLevel::L1,
      "The full 64-bit pattern-match state machine on Memory::interceptRead",
      "Nothing below the pattern match exists.", "NoSlotClock.h/.cpp" },

    // ── Off-axis ────────────────────────────────────────────────────────
    { "rewind", "Host-side (off-axis)", "Rewind / snapshot", AbsLevel::Host,
      "Keyframes + XOR deltas at frame boundaries — reaching into VIA, AY and "
      "SSI263 state so music and speech survive a rewind",
      "No hardware referent: real Apple IIs do not rewind.",
      "RewindBuffer.*, MachineSnapshot.*" },
    { "turbo", "Host-side (off-axis)", "Disk turbo (~60x) and MAX speed",
      AbsLevel::Host,
      "Pure host pacing — and precisely why emuCycles stamping is mandatory "
      "everywhere: turbo collapses wall-clock gaps to zero",
      "No hardware referent.", "EmulationController.h/.cpp" },
    { "aiserver", "Host-side (off-axis)", "AI control server", AbsLevel::Host,
      "Out-of-band agent channel over loopback HTTP", "No hardware referent.",
      "AiControlServer.h/.cpp" },
    { "crt", "Host-side (off-axis)", "CRT effect stack / 3D voxel / Paint",
      AbsLevel::Host,
      "Presentation and authoring layers above the framebuffer",
      "No hardware referent — a real CRT's glass is not a bus device.",
      "CrtEffectStack.*, Voxel3DRenderer.*, hgrpaint/*" },
    { "bootfromslot", "Host-side (off-axis)", "bootFromSlot shortcut",
      AbsLevel::Host,
      "Cold boot plus a forced PC = $Cn00 after validating the JSR trio — no "
      "real firmware scan happens",
      "Labelled a synthetic shortcut in EmulationController.h.",
      "EmulationController.cpp" },
    };
    return kCatalog;
}

// ─── The window ──────────────────────────────────────────────────────────

AbstractionLevels_ImGui::Request
AbstractionLevels_ImGui::render(bool* open, const Snapshot& snap)
{
    Request req;
    if (!open || !*open) return req;

    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_LAYER_GROUP " Abstraction Levels (LLE / HLE)"
                      "###abstractionLevels", open)) {
        ImGui::End();
        return req;
    }

    const Palette& pal = palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    // ── What the axis means ─────────────────────────────────────────────
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped(
        "Where POM2 cuts the emulation boundary. "
        "%s LLE cuts at the chip's pins: internal state machines and timing "
        "are modelled and the guest's own firmware runs on top. "
        "%s HLE cuts at the service: POM2 intercepts a call or a byte stream "
        "and produces the right result on the host.",
        levelBadge(AbsLevel::L0), levelBadge(AbsLevel::H1));
    ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
    ImGui::TextWrapped(
        "They fail differently, which is the useful part: LLE fails by being "
        "INCOMPLETE (an unmodelled edge hangs the firmware), HLE fails by "
        "being OUT OF CONTRACT (software pokes a register the abstraction "
        "never had, gets a plausible wrong answer, and nothing hangs). "
        "Full reasoning, with evidence: docs/lle_vs_hle.md.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    // ── Legend ──────────────────────────────────────────────────────────
    ImGui::Spacing();
    {
        static const AbsLevel kAll[] = { AbsLevel::L0, AbsLevel::L1,
                                         AbsLevel::L2, AbsLevel::H1,
                                         AbsLevel::H2 };
        static const char* kTips[] = {
            "Silicon. Internal state machine + cycle timing modelled; a real\n"
            "ROM/firmware dump executes on top and cannot tell the difference.",
            "Chip-faithful. Full register/protocol model at bus timing;\n"
            "firmware-invisible internals deliberately skipped.",
            "Real firmware, host device. The card's real ROM executes, but\n"
            "what it drives is a host implementation. POM2's preferred\n"
            "compromise whenever a dump exists but the chip need not.",
            "Synthetic firmware. POM2 hand-assembles a slot ROM: the guest\n"
            "6502 code is real, the register protocol behind it is invented,\n"
            "and the work happens on the host.",
            "Host function. No guest-visible hardware at all — the function\n"
            "happens on the host and the result appears out-of-band."
        };
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            levelChip(kAll[i]);
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(u32(levelIsLle(kAll[i]) ? pal.text : pal.textDim),
                               "%s", levelName(kAll[i]));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTips[i]);
            if (i == 2) {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
            }
        }
    }

    // ── Switchable boundaries ───────────────────────────────────────────
    //
    // Four subsystems ship both levels behind one interface. That is policy,
    // not accident ("when both levels have real users, ship both"), but until
    // now the choice was expressed as a catalog key in Slot Configuration —
    // you had to already know that `mouseaw` meant "the HLE one". Here the
    // choice IS the level, with the cost of each side spelled out.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(ICON_FA_TOGGLE_ON " Switchable boundaries",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (snap.toggles.empty())
            ImGui::TextDisabled("None available on this machine.");

        for (const ToggleState& t : snap.toggles) {
            ImGui::PushID(static_cast<int>(t.id));
            ImGui::TextUnformatted(t.title.c_str());
            ImGui::SameLine();
            if (t.needsRestart)
                ImGui::TextColored(u32(pal.warn), "(restarts the machine)");
            else
                ImGui::TextColored(u32(pal.ok), "(takes effect immediately)");

            auto side = [&](const ToggleOption& o, int idx) {
                const bool on = (t.selected == idx);
                ImGui::BeginDisabled(!o.available);
                char label[160];
                std::snprintf(label, sizeof(label), "%s  %s",
                              levelBadge(o.level), o.label.c_str());
                // Radio-style: the two sides are exclusive and one is live,
                // so a pair of buttons would leave "which am I on?" to colour
                // alone. RadioButton says it structurally.
                if (ImGui::RadioButton(label, on) && !on && o.available) {
                    req.toggle = t.id;
                    req.option = idx;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                    if (!o.available && !o.blockedBy.empty())
                        ImGui::SetTooltip("Unavailable — %s", o.blockedBy.c_str());
                    else if (!o.why.empty())
                        ImGui::SetTooltip("%s", o.why.c_str());
                }
            };
            ImGui::Indent();
            side(t.low, 0);
            side(t.high, 1);
            if (!t.note.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
                ImGui::TextWrapped("%s", t.note.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Unindent();
            ImGui::Spacing();
            ImGui::PopID();
        }
    }

    // ── Filters ─────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Every subsystem");
    ImGui::SetNextItemWidth(16.0f * ImGui::GetFontSize());
    ImGui::Combo("##half", &halfFilter_,
                 "Everything\0LLE only (L0-L2)\0HLE only (H1-H2)\0"
                 "Host-side (off-axis)\0");
    ImGui::SameLine();
    ImGui::Checkbox("Live only", &onlyLive_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide subsystems with no card on the bus right now.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Filter...", search_, sizeof(search_));

    // Index the live rows once per frame — the catalog is walked in group
    // order and a linear scan per row would be quadratic for no reason.
    std::unordered_map<std::string, const Row*> live;
    for (const Row& r : snap.rows) live[r.id] = &r;

    auto matches = [&](const AbsEntry& e, const Row* r) {
        if (halfFilter_ == 1 && !levelIsLle(e.level))           return false;
        if (halfFilter_ == 2 && (levelIsLle(e.level) ||
                                 e.level == AbsLevel::Host))     return false;
        if (halfFilter_ == 3 && e.level != AbsLevel::Host)       return false;
        if (onlyLive_ && r && r->live == Live::NotPlugged)       return false;
        if (search_[0]) {
            // Case-insensitive substring over the two columns the user is
            // most likely to be hunting in.
            auto hay = std::string(e.subsystem) + " " + e.modelled + " " +
                       e.group + " " + e.files;
            std::string needle(search_);
            auto lower = [](std::string v) {
                for (char& c : v) c = static_cast<char>(std::tolower(
                                          static_cast<unsigned char>(c)));
                return v;
            };
            if (lower(hay).find(lower(needle)) == std::string::npos)
                return false;
        }
        return true;
    };

    int lleCount = 0, hleCount = 0, degraded = 0, shown = 0;

    if (ImGui::BeginTable("##abs", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Level",  ImGuiTableColumnFlags_WidthFixed,
                                3.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Now",    ImGuiTableColumnFlags_WidthFixed,
                                7.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("What is actually modelled",
                                ImGuiTableColumnFlags_WidthStretch, 0.60f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const char* group = nullptr;
        for (const AbsEntry& e : abstractionCatalog()) {
            auto it = live.find(e.id);
            const Row* r = (it != live.end()) ? it->second : nullptr;
            if (!matches(e, r)) continue;
            ++shown;
            if (levelIsLle(e.level))          ++lleCount;
            else if (e.level != AbsLevel::Host) ++hleCount;
            if (r && r->live == Live::Degraded) ++degraded;

            if (!group || std::strcmp(group, e.group) != 0) {
                group = e.group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.accentDim));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(group);
                ImGui::PopStyleColor();
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            levelChip(e.level);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.subsystem);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(30.0f * ImGui::GetFontSize());
                ImGui::TextColored(u32(levelColour(e.level)), "%s — %s",
                                   levelBadge(e.level), levelName(e.level));
                ImGui::Separator();
                ImGui::TextWrapped("Why not lower: %s", e.whyNot);
                ImGui::Spacing();
                ImGui::TextColored(u32(pal.textDim), "%s", e.files);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(2);
            if (!r) {
                ImGui::TextColored(u32(pal.textDim), "—");
            } else switch (r->live) {
                case Live::Active:
                    ImGui::TextColored(u32(pal.ok), ICON_FA_CIRCLE_CHECK " live");
                    break;
                case Live::Degraded:
                    // The whole reason this column exists. A degraded path is
                    // NOT an error — the machine works — which is exactly why
                    // it needs saying out loud.
                    ImGui::TextColored(u32(pal.warn),
                                       ICON_FA_TRIANGLE_EXCLAMATION " %s now",
                                       levelBadge(r->actual));
                    break;
                case Live::NotPlugged:
                    ImGui::TextColored(u32(pal.textDim), "not plugged");
                    break;
                case Live::NotApplicable:
                    ImGui::TextColored(u32(pal.ok), ICON_FA_CIRCLE_CHECK " live");
                    break;
            }
            if (r && !r->detail.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r->detail.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", e.modelled);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.textDim));
    ImGui::Text("%d shown — %d LLE, %d HLE.", shown, lleCount, hleCount);
    ImGui::PopStyleColor();
    if (degraded > 0) {
        ImGui::SameLine();
        ImGui::TextColored(u32(pal.warn),
                           ICON_FA_TRIANGLE_EXCLAMATION
                           " %d running above its catalogued level "
                           "(a ROM dump is missing — see ROM Status).",
                           degraded);
    }

    ImGui::End();
    return req;
}

} // namespace pom2
