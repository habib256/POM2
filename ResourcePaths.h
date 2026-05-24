// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ResourcePaths — one place that knows where POM2's bundled read-only
// assets (roms/, fonts/, roms/floppy_samples/, …) live at runtime.
//
// Historically every asset-loading site repeated the same idiom:
//   probe `roms/X`, then `../roms/X`, then `../../roms/X`
// which only works when the current working directory is the repo root
// (dev: run_emulator.sh) or one/two levels below it (dev: launched from
// build/). That breaks the moment POM2 is *installed* or *bundled* and
// launched by an absolute path with an unrelated CWD — exactly what a
// release does (AppImage, .deb → /usr/bin/POM2, a desktop launcher, or a
// portable tarball double-clicked from a file manager).
//
// This helper adds executable-relative and FHS-install search roots on
// top of the legacy CWD-relative ones, so the same `findResource("roms/
// apple2.rom")` call resolves in dev, in a portable bundle, and in an
// FHS install — without any call site having to know which.

#ifndef POM2_RESOURCE_PATHS_H
#define POM2_RESOURCE_PATHS_H

#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

/// Absolute directory holding the running executable, or an empty path
/// when it can't be determined. Cached after the first call. Platform
/// back-ends: `/proc/self/exe` (Linux), `_NSGetExecutablePath` (macOS),
/// `GetModuleFileNameW` (Windows).
std::filesystem::path executableDir();

/// Ordered, de-duplicated base directories searched for bundled assets.
/// Search order (first hit wins):
///   1. CWD                        — dev (run_emulator.sh from repo root)
///   2. ../, ../../               — dev (launched from build/ or build/<cfg>/)
///   3. <exeDir>                  — portable bundle (binary beside roms/)
///   4. <exeDir>/..               — portable bundle (binary in bin/)
///   5. <exeDir>/../share/POM2    — FHS install (/usr/bin + /usr/share/POM2)
/// Cached after the first call.
const std::vector<std::filesystem::path>& resourceSearchDirs();

/// Resolve a relative asset path (e.g. "roms/apple2.rom") against every
/// search dir, returning the first existing one (as a string usable by
/// std::ifstream / Memory). An absolute `rel` that exists is returned
/// unchanged. Returns "" when nothing matches.
std::string findResource(const std::string& rel);

/// First of `candidates` (each tried via findResource) that resolves to
/// an existing file; "" when none do. For probe-order lists.
std::string findFirstResource(const std::vector<std::string>& candidates);

} // namespace pom2

#endif // POM2_RESOURCE_PATHS_H
