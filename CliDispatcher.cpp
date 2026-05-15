// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CliDispatcher.cpp — verb parser + Phase-C runner. Inspired by POM1's
// CliDispatcher with the POM1-card-specific verbs (sid, jukebox, codetank,
// microsd, tms9918, …) removed.

#include "CliDispatcher.h"

#include "EmulationController.h"
#include "Logger.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace pom2 {
namespace {

bool endsWithIcase(const std::string& s, std::string_view suffix)
{
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i]))
            != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}

/// Parse "0x0300", "$0300", "0300", or "768" → 16-bit address.
bool parseAddr16(const std::string& s, int& out)
{
    if (s.empty()) return false;
    std::string v = s;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.erase(0, 2);
    } else if (v[0] == '$') {
        v.erase(0, 1);
    } else {
        bool hasHexLetter = false;
        for (char c : v) {
            if (std::isxdigit(static_cast<unsigned char>(c)) &&
                !std::isdigit(static_cast<unsigned char>(c))) hasHexLetter = true;
        }
        if (!hasHexLetter && v.size() > 4) {
            try {
                long n = std::stol(v, nullptr, 10);
                if (n < 0 || n > 0xFFFF) return false;
                out = static_cast<int>(n);
                return true;
            } catch (...) { return false; }
        }
    }
    if (v.empty() || v.size() > 4) return false;
    try {
        long n = std::stol(v, nullptr, 16);
        if (n < 0 || n > 0xFFFF) return false;
        out = static_cast<int>(n);
        return true;
    } catch (...) { return false; }
}

bool parseIntPositive(const std::string& s, int& out)
{
    try {
        size_t idx = 0;
        long n = std::stol(s, &idx, 10);
        if (idx != s.size() || n < 0) return false;
        out = static_cast<int>(n);
        return true;
    } catch (...) { return false; }
}

bool splitOnColon(const std::string& s, std::string& l, std::string& r)
{
    auto pos = s.find(':');
    if (pos == std::string::npos) return false;
    l = s.substr(0, pos);
    r = s.substr(pos + 1);
    return !l.empty() && !r.empty();
}

bool parsePresetName(const std::string& raw, CliPreset& out)
{
    std::string s = raw;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "ii"  || s == "apple2"  || s == "appleii")  { out = CliPreset::AppleII;     return true; }
    if (s == "ii+" || s == "iiplus" || s == "apple2plus" ||
        s == "appleiiplus" || s == "ii-plus") { out = CliPreset::AppleIIPlus; return true; }
    if (s == "iie" || s == "apple2e" || s == "appleiie" ||
        s == "//e" || s == "iie-enhanced")  { out = CliPreset::AppleIIe;    return true; }
    if (s == "iic" || s == "apple2c" || s == "appleiic" ||
        s == "//c")                         { out = CliPreset::AppleIIc;    return true; }
    if (s == "iic+" || s == "iicplus" || s == "apple2cplus" ||
        s == "apple2cp" || s == "appleiicplus" ||
        s == "//c+")                        { out = CliPreset::AppleIIcPlus; return true; }
    return false;
}

void printUsage()
{
    std::fprintf(stderr,
        "Usage: POM2 [options]\n"
        "\n"
        "Phase-A boot options (consumed before MainWindow starts):\n"
        "  -p, --preset <ii|ii+|iie|iic|iic+>  System profile to boot into\n"
        "  --ii-plus                  Force II+ mode (ignore roms/apple2e.rom)\n"
        "  --speed <cycles/frame>     Override CPU pacing (1x = 17045)\n"
        "  --cpu-max                  Run flat-out (~58 MHz emulated)\n"
        "  --tape <path>              Preload + auto-play tape\n"
        "  --35-disk1 <path>          Mount 800K 3.5\" image in //c+ internal drive\n"
        "  --35-disk2 <path>          Mount 800K 3.5\" image in //c+ external drive\n"
        "  --save-tape <path>         Dump captured tape on shutdown\n"
        "  --save-tape-format <aci|wav>   Force save extension\n"
        "  --snapshot-load <path>     Restore state at boot (Phase C)\n"
        "  --display <mode>           Hi-res render mode at boot. mode = ntsc | chatmauve\n"
        "                              | mono-white | mono-green | mono-amber\n"
        "\n"
        "Phase-C deferred (run in CLI order after a short settle):\n"
        "  --load <addr>:<path>       Load raw binary at addr\n"
        "  --run <addr>               Jump to addr\n"
        "  --paste <file>             Feed file contents to keyboard (<=4096)\n"
        "  --step <N>                 Single-step N instructions\n"
        "  --trace-brk                Enable BRK register dump\n"
        "  --play / --rec / --rewind  Cassette transport\n"
        "  --snapshot-save <path>     Write snapshot\n"
        "\n"
        "  -h, --help                 Show this and exit\n");
}

} // namespace

std::string resolveSaveTapePath(const std::string& path, CliSaveTapeFormat hint)
{
    if (path.empty()) return path;
    const bool hasAci = endsWithIcase(path, ".aci");
    const bool hasWav = endsWithIcase(path, ".wav");
    if (hasAci || hasWav) return path;
    switch (hint) {
        case CliSaveTapeFormat::Aci: return path + ".aci";
        case CliSaveTapeFormat::Wav: return path + ".wav";
        case CliSaveTapeFormat::NoHint: default: return path + ".aci";
    }
}

std::optional<CliPlan> parseCli(int argc, char* argv[], bool& helpRequestedOut)
{
    helpRequestedOut = false;
    CliPlan plan;

    auto needArg = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            pom2::log().error("CLI", std::string(flag) + " requires an argument");
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            printUsage();
            helpRequestedOut = true;
            return std::nullopt;
        }
        if (a == "-p" || a == "--preset") {
            const char* v = needArg(i, "--preset"); if (!v) return std::nullopt;
            if (!parsePresetName(v, plan.preset)) {
                pom2::log().error("CLI", std::string("unknown preset: ") + v);
                return std::nullopt;
            }
        }
        else if (a == "--cpu-max") {
            plan.cpuMax = true;
        }
        else if (a == "--ii-plus" || a == "--ii+") {
            plan.forceIIPlus = true;
        }
        else if (a == "--display") {
            const char* v = needArg(i, "--display"); if (!v) return std::nullopt;
            const std::string s = v;
            if      (s == "ntsc"        || s == "color-ntsc")  plan.displayMode = CliDisplayMode::ColorNTSC;
            else if (s == "chatmauve"   || s == "chat-mauve"
                  || s == "rgb"         || s == "le-chat-mauve") plan.displayMode = CliDisplayMode::ChatMauveRGB;
            else if (s == "mono-white"  || s == "white")       plan.displayMode = CliDisplayMode::MonoWhite;
            else if (s == "mono-green"  || s == "green"
                  || s == "p31")                                plan.displayMode = CliDisplayMode::MonoGreen;
            else if (s == "mono-amber"  || s == "amber")       plan.displayMode = CliDisplayMode::MonoAmber;
            else {
                pom2::log().error("CLI", std::string("unknown --display mode: ") + v
                    + " (expected ntsc|chatmauve|mono-white|mono-green|mono-amber)");
                return std::nullopt;
            }
        }
        else if (a == "--speed") {
            const char* v = needArg(i, "--speed"); if (!v) return std::nullopt;
            int n; if (!parseIntPositive(v, n) || n <= 0) {
                pom2::log().error("CLI", std::string("invalid --speed: ") + v);
                return std::nullopt;
            }
            plan.executionSpeed = n;
        }
        else if (a == "--35-disk1") {
            const char* v = needArg(i, "--35-disk1"); if (!v) return std::nullopt;
            plan.disk35Internal = v;
        }
        else if (a == "--35-disk2") {
            const char* v = needArg(i, "--35-disk2"); if (!v) return std::nullopt;
            plan.disk35External = v;
        }
        else if (a == "--tape") {
            const char* v = needArg(i, "--tape"); if (!v) return std::nullopt;
            plan.initialTapePath = v;
            plan.initialTapeAutoPlay = true;
        }
        else if (a == "--save-tape") {
            const char* v = needArg(i, "--save-tape"); if (!v) return std::nullopt;
            plan.saveTapePath = v;
        }
        else if (a == "--save-tape-format") {
            const char* v = needArg(i, "--save-tape-format"); if (!v) return std::nullopt;
            std::string s = v;
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if      (s == "aci") plan.saveTapeFormat = CliSaveTapeFormat::Aci;
            else if (s == "wav") plan.saveTapeFormat = CliSaveTapeFormat::Wav;
            else {
                pom2::log().error("CLI", std::string("--save-tape-format expects aci|wav, got: ") + v);
                return std::nullopt;
            }
        }
        else if (a == "--load") {
            const char* v = needArg(i, "--load"); if (!v) return std::nullopt;
            std::string addrStr, path;
            if (!splitOnColon(v, addrStr, path)) {
                pom2::log().error("CLI", std::string("--load expects ADDR:PATH, got: ") + v);
                return std::nullopt;
            }
            int addr;
            if (!parseAddr16(addrStr, addr)) {
                pom2::log().error("CLI", std::string("--load address parse failed: ") + addrStr);
                return std::nullopt;
            }
            CliAction act{};
            act.kind = CliAction::Kind::Load;
            act.addressI = addr;
            act.pathS = path;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--run") {
            const char* v = needArg(i, "--run"); if (!v) return std::nullopt;
            int addr;
            if (!parseAddr16(v, addr)) {
                pom2::log().error("CLI", std::string("--run address parse failed: ") + v);
                return std::nullopt;
            }
            CliAction act{}; act.kind = CliAction::Kind::Run; act.addressI = addr;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--paste") {
            const char* v = needArg(i, "--paste"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::Paste; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--step") {
            const char* v = needArg(i, "--step"); if (!v) return std::nullopt;
            int n;
            if (!parseIntPositive(v, n) || n <= 0) {
                pom2::log().error("CLI", std::string("--step expects positive int, got: ") + v);
                return std::nullopt;
            }
            CliAction act{}; act.kind = CliAction::Kind::Step; act.countI = n;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--trace-brk") {
            CliAction act{}; act.kind = CliAction::Kind::TraceBrk;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--play") {
            CliAction act{}; act.kind = CliAction::Kind::PlayTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--rec") {
            CliAction act{}; act.kind = CliAction::Kind::RecTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--rewind") {
            CliAction act{}; act.kind = CliAction::Kind::RewindTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--snapshot-save") {
            const char* v = needArg(i, "--snapshot-save"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::SnapshotSave; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--snapshot-load") {
            const char* v = needArg(i, "--snapshot-load"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::SnapshotLoad; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else {
            pom2::log().error("CLI", std::string("unknown flag: ") + a);
            printUsage();
            return std::nullopt;
        }
    }

    return plan;
}

namespace {

/// Read a file and feed it through Memory::pasteText, which normalises
/// line-endings (\r\n / \r / \n → CR) and drains via the strobe-aware
/// queue (one byte per $C010 clear). Capped at Memory::kPasteMaxChars.
void runPasteFile(const std::string& path, EmulationController& emu)
{
    std::ifstream f(path);
    if (!f) {
        pom2::log().error("CLI", "--paste cannot open " + path);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    const size_t queued = emu.memory().pasteText(content);
    pom2::log().info("CLI", "--paste queued " + std::to_string(queued) +
                            " chars from " + path);
}

void runLoad(const CliAction& a, EmulationController& emu)
{
    std::ifstream f(a.pathS, std::ios::binary);
    if (!f) {
        pom2::log().error("CLI", "--load cannot open " + a.pathS);
        return;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        pom2::log().error("CLI", "--load file is empty: " + a.pathS);
        return;
    }
    if (static_cast<size_t>(a.addressI) + bytes.size() > 0x10000) {
        pom2::log().error("CLI", "--load overflows $FFFF");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(emu.stateMutex());
        for (size_t i = 0; i < bytes.size(); ++i) {
            emu.memory().memWrite(static_cast<uint16_t>(a.addressI + i), bytes[i]);
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "--load wrote %zu bytes at $%04X (from %s)",
                  bytes.size(), a.addressI, a.pathS.c_str());
    pom2::log().info("CLI", buf);
}

} // namespace

void runDeferredActions(const std::vector<CliAction>& actions,
                        EmulationController& emu)
{
    for (const CliAction& a : actions) {
        switch (a.kind) {
            case CliAction::Kind::Load:
                runLoad(a, emu);
                break;
            case CliAction::Kind::Run: {
                std::lock_guard<std::mutex> lk(emu.stateMutex());
                emu.cpu().setProgramCounter(static_cast<uint16_t>(a.addressI));
                emu.setMode(EmulationController::Mode::Running);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "--run jumped to $%04X", a.addressI);
                pom2::log().info("CLI", buf);
                break;
            }
            case CliAction::Kind::Paste:
                runPasteFile(a.pathS, emu);
                break;
            case CliAction::Kind::Step: {
                emu.setMode(EmulationController::Mode::Stopped);
                for (int n = 0; n < a.countI; ++n) emu.requestStep();
                pom2::log().info("CLI", "--step requested " + std::to_string(a.countI));
                break;
            }
            case CliAction::Kind::TraceBrk:
                pom2::log().info("CLI", "--trace-brk: not yet wired in M6502");
                break;
            case CliAction::Kind::PlayTape:
                emu.playTape();
                pom2::log().info("CLI", "--play: tape rolling");
                break;
            case CliAction::Kind::RecTape:
                emu.cassette().armRecording();
                pom2::log().info("CLI", "--rec: cassette capture armed");
                break;
            case CliAction::Kind::RewindTape:
                emu.rewindTape();
                pom2::log().info("CLI", "--rewind: tape rewound");
                break;
            case CliAction::Kind::SnapshotSave:
                pom2::log().info("CLI", "--snapshot-save: not yet wired in MainWindow");
                break;
            case CliAction::Kind::SnapshotLoad:
                pom2::log().info("CLI", "--snapshot-load: not yet wired in MainWindow");
                break;
        }
    }
}

} // namespace pom2
