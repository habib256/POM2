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

// Minimal levelled logger — thread-safe stderr sink with a tag per message.
// Used everywhere a subsystem wants to log something:
//   pom2::log().info("ROM", "Loaded apple2.rom");

#ifndef POM2_LOGGER_H
#define POM2_LOGGER_H

#include <cstdio>
#include <mutex>
#include <string>

namespace pom2 {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger
{
public:
    void log(LogLevel level, const char* tag, const std::string& msg) {
        static const char* names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
        std::lock_guard<std::mutex> lk(mtx);
        std::fprintf(stderr, "[%s] %s: %s\n",
                     names[static_cast<int>(level)], tag, msg.c_str());
    }
    void debug(const char* tag, const std::string& m) { log(LogLevel::Debug, tag, m); }
    void info (const char* tag, const std::string& m) { log(LogLevel::Info,  tag, m); }
    void warn (const char* tag, const std::string& m) { log(LogLevel::Warn,  tag, m); }
    void error(const char* tag, const std::string& m) { log(LogLevel::Error, tag, m); }
private:
    std::mutex mtx;
};

inline Logger& log() { static Logger g; return g; }

} // namespace pom2

#endif // POM2_LOGGER_H
