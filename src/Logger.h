// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
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
