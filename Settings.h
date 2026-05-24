// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Settings — minimal "remember runtime config across sessions" facility.
//
// Stored at `${HOME}/.config/POM2/state.cfg` (or `${HOME}/.pom2_state`
// fallback when XDG dirs aren't available; or in `./pom2_state.cfg` when
// HOME itself is unset). Plain `key=value` lines, one per setting.
// Unknown keys are preserved on round-trip so a future binary that drops
// a setting doesn't lose user data; unknown keys are simply ignored on
// the get-side.
//
// Loading is lossy-safe: a missing file, malformed line, or unparseable
// value all fall back to the caller's default. The host (MainWindow) is
// responsible for sanity-checking each path it reads (e.g. mount only if
// the file still exists).
//
// Saving is atomic: writes to `state.cfg.tmp` first, then renames over
// the live file, so a crash mid-write never corrupts the existing config.

#ifndef POM2_SETTINGS_H
#define POM2_SETTINGS_H

#include <map>
#include <string>

namespace pom2 {

class Settings
{
public:
    /// Load `state.cfg` from the well-known location. Missing or
    /// malformed file → empty store (defaults will apply at the call
    /// site). Returns true if a file was successfully read; false on
    /// missing-file (not an error condition).
    bool load();

    /// Persist the current key/value store. Atomic rename. Returns true
    /// on success; logs a warning on failure (no exceptions).
    bool save() const;

    /// Get a value or fall back to the default. Conversion failures
    /// also fall back. Booleans accept "true"/"false"/"1"/"0".
    std::string getString(const std::string& key, const std::string& def = "") const;
    bool        getBool  (const std::string& key, bool        def = false) const;
    int         getInt   (const std::string& key, int         def = 0)     const;
    float       getFloat (const std::string& key, float       def = 0.0f)  const;

    void setString(const std::string& key, std::string value);
    void setBool  (const std::string& key, bool   value);
    void setInt   (const std::string& key, int    value);
    void setFloat (const std::string& key, float  value);

    /// Resolved file path (visible in About / log).
    std::string getStorePath() const;

private:
    std::map<std::string, std::string> store;
};

} // namespace pom2

#endif // POM2_SETTINGS_H
