// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Runtime-only host services for FujiNet. Process discovery/supervision stays
// out of FujiNetCard and the frontend sees one FujiNet-specific lifecycle API.

#ifndef POM2_FUJINET_HOST_H
#define POM2_FUJINET_HOST_H

#include <memory>
#include <string>

namespace pom2 {

class ChildProcess;

class FujiNetHost
{
public:
    FujiNetHost();
    ~FujiNetHost();

    FujiNetHost(const FujiNetHost&) = delete;
    FujiNetHost& operator=(const FujiNetHost&) = delete;

    /// Resolve a configured name/path using ChildProcess's portable search.
    /// An empty setting searches for the conventional `fujinet` executable.
    static std::string resolveHelper(const std::string& configuredPath);

    bool startHelper(const std::string& configuredPath, std::string& errOut);
    void stopHelper();
    bool helperRunning();
    int  helperExitCode() const;

private:
    std::unique_ptr<ChildProcess> helper_;
};

} // namespace pom2

#endif // POM2_FUJINET_HOST_H
