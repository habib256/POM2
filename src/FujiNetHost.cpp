// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "FujiNetHost.h"

#include "ChildProcess.h"

namespace pom2 {

FujiNetHost::FujiNetHost() : helper_(std::make_unique<ChildProcess>()) {}

FujiNetHost::~FujiNetHost() { stopHelper(); }

std::string FujiNetHost::resolveHelper(const std::string& configuredPath)
{
    return ChildProcess::findOnPath(configuredPath.empty()
                                        ? std::string("fujinet")
                                        : configuredPath);
}

bool FujiNetHost::startHelper(const std::string& configuredPath,
                              std::string& errOut)
{
    std::string exe = configuredPath;
    if (exe.empty()) exe = resolveHelper({});
    if (exe.empty()) {
        errOut = "no FujiNet program found — set its path, or install one on "
                 "PATH as 'fujinet'";
        return false;
    }

    // No arguments: the firmware reads the Bus-over-IP target from its own
    // fnconfig.ini (whose Apple default is 127.0.0.1:1985).
    return helper_->start(exe, {}, std::string{}, errOut);
}

void FujiNetHost::stopHelper() { helper_->stop(); }

bool FujiNetHost::helperRunning() { return helper_->isRunning(); }

int FujiNetHost::helperExitCode() const { return helper_->lastExitCode(); }

} // namespace pom2
