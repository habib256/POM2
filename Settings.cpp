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

} // namespace

bool Settings::load()
{
    const fs::path path = resolveStorePath();
    std::ifstream f(path);
    if (!f) return false;     // missing → use defaults; not an error

    std::string line;
    while (std::getline(f, line)) {
        // Strip comments (anything after the first '#') and trim.
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key   = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
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
            f << kv.first << '=' << kv.second << '\n';
        }
        if (!f) {
            pom2::log().warn("Settings", "Short write on " + tmp.string());
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
