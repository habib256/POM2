// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Settings.h"
#include "Logger.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pom2 {

namespace fs = std::filesystem;

namespace {

// Resolve the per-user POM2 state directory. Mirrors the XDG basedir
// convention on Unix; on Windows we'd use %APPDATA%, but POM2 is
// cross-platform-best-effort and a simple HOME-based path is enough.
fs::path resolveStorePath()
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        // Fall back to the working directory.
        return fs::path("pom2_state.cfg");
    }
    fs::path xdg = fs::path(home) / ".config" / "POM2";
    std::error_code ec;
    fs::create_directories(xdg, ec);
    if (!ec && fs::is_directory(xdg, ec)) {
        return xdg / "state.cfg";
    }
    // create_directories failed — last resort, dotfile in $HOME.
    return fs::path(home) / ".pom2_state";
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Escape the record-separator and the escape char so arbitrary string values
// round-trip through the line-oriented `key=value` store: a newline would
// otherwise split one entry into two (the second dropped as a no-`=` line),
// and a backslash is escaped to keep the encoding unambiguous. ('#' does NOT
// need escaping — load treats it as a comment only at the start of a line.)
std::string escapeValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string unescapeValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            if      (n == 'n')  out += '\n';
            else if (n == 'r')  out += '\r';
            else if (n == '\\') out += '\\';
            else { out += '\\'; out += n; }   // unknown escape — pass through
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

bool Settings::load()
{
    const fs::path path = resolveStorePath();
    std::ifstream f(path);
    if (!f) return false;     // missing → use defaults; not an error

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        // A '#' is a comment marker ONLY at the start of a line — stripping
        // after the first '#' anywhere would truncate any value that legally
        // contains '#' (e.g. a disk path "/home/u/My#Disks/game.dsk").
        if (line.empty() || line[0] == '#') continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key   = unescapeValue(trim(line.substr(0, eq)));
        const std::string value = unescapeValue(trim(line.substr(eq + 1)));
        if (!key.empty()) store[key] = value;
    }
    pom2::log().info("Settings",
        "Loaded " + std::to_string(store.size()) + " keys from " + path.string());
    return true;
}

bool Settings::save() const
{
    const fs::path path = resolveStorePath();
    const fs::path tmp  = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            pom2::log().warn("Settings",
                "Cannot open " + tmp.string() + " for write");
            return false;
        }
        f << "# POM2 runtime config — written automatically on exit.\n";
        f << "# Edit by hand at your own risk; unknown keys are preserved.\n";
        for (const auto& kv : store) {
            f << escapeValue(kv.first) << '=' << escapeValue(kv.second) << '\n';
        }
        // Flush + close BEFORE the rename so a deferred write error (disk full /
        // quota) is observed here — checking the stream while it's still open
        // misses the failure and would rename a truncated .tmp over the good
        // config, defeating the whole atomic-write dance.
        f.flush();
        f.close();
        if (!f) {
            pom2::log().warn("Settings", "Write/flush failed on " + tmp.string());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        pom2::log().warn("Settings",
            "Rename " + tmp.string() + " → " + path.string() + " failed: " + ec.message());
        return false;
    }
    return true;
}

std::string Settings::getString(const std::string& key, const std::string& def) const
{
    auto it = store.find(key);
    return it == store.end() ? def : it->second;
}

bool Settings::getBool(const std::string& key, bool def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    const std::string& v = it->second;
    if (v == "true" || v == "1" || v == "yes" || v == "on")  return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
}

int Settings::getInt(const std::string& key, int def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

float Settings::getFloat(const std::string& key, float def) const
{
    auto it = store.find(key);
    if (it == store.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

void Settings::setString(const std::string& key, std::string value) { store[key] = std::move(value); }
void Settings::setBool  (const std::string& key, bool  v) { store[key] = v ? "true" : "false"; }
void Settings::setInt   (const std::string& key, int   v) { store[key] = std::to_string(v); }
void Settings::setFloat (const std::string& key, float v)
{
    std::ostringstream os; os << v; store[key] = os.str();
}

std::string Settings::getStorePath() const { return resolveStorePath().string(); }

} // namespace pom2
