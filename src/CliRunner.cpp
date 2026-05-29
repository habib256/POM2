// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CliRunner.cpp — Phase-C deferred-action runner. Split out of
// CliDispatcher.cpp (2026-05-23) so the *parser* (`parseCli`) stays free of
// any EmulationController dependency and can be unit-tested without linking
// the whole emulation core. This TU is the only half that touches the live
// machine, so it carries the EmulationController include.

#include "CliDispatcher.h"

#include "EmulationController.h"
#include "Logger.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace pom2 {
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
    // Read at most the paste-queue cap so a huge/unbounded source (e.g.
    // /dev/zero) can't exhaust memory before pasteText() applies its cap —
    // pasteText discards anything beyond Memory::kPasteMaxChars anyway.
    static constexpr size_t kMaxPaste = 4096;  // == Memory::kPasteMaxChars
    char buf[kMaxPaste];
    f.read(buf, sizeof(buf));
    const std::string content(buf, static_cast<size_t>(f.gcount()));
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
    // Reject oversized sources before allocating, so an unbounded file (e.g.
    // /dev/zero) or a multi-GB file can't exhaust memory. A 6502 image can be
    // at most 64 KiB; the address+size>0x10000 check below still applies.
    f.seekg(0, std::ios::end);
    const std::streamoff fsz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fsz > 0x10000) {
        pom2::log().error("CLI", "--load file exceeds 64 KiB: " + a.pathS);
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
                emu.requestStep(a.countI);   // queues N steps (counter, not coalesced)
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
                emu.armRecording();   // locked wrapper — avoids racing the CPU worker
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
