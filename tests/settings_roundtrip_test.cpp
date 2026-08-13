// Settings round-trip regression test (round 10 #4/#5).
//
// The line-oriented key=value store must round-trip ARBITRARY string values:
//   #4 a value containing '#' was truncated on reload (load stripped after
//      the first '#' anywhere) — silently breaking disk paths like
//      "/home/u/My#Disks/game.dsk".
//   #5 a value containing a newline split into two lines, the second dropped.
// Both are fixed: '#' is a comment only at line start, and values are
// escaped (\\, \n, \r) on save / unescaped on load.
//
// Extended 2026-07-30 with the TYPED accessors, which this file previously only
// spot-checked as raw strings — and floats did not in fact round-trip:
// `ostringstream` defaults to 6 significant digits, so 1.0f/3.0f wrote as
// "0.333333" and read back as a DIFFERENT float. Every persisted float (all
// five volumes, ui_scale, and the ~15 NTSC/CRT + voxel shader parameters) thus
// shifted on the first save/load cycle. setFloat now emits the shortest width
// that round-trips, capped at max_digits10.
//
// Also pins BOUNDARY whitespace: load() trims each line (to drop a CRLF '\r'
// artifact), so a value with leading/trailing space or tab only survives
// because escapeValue encodes those positions specially.
//
// Drives save()→load() through a real file by pointing HOME at a temp dir.

#include "Settings.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / "pom2_settings_rt_home";
    fs::remove_all(home);
    fs::create_directories(home);
    ::setenv("HOME", home.string().c_str(), 1);

    const std::string kHash   = "/home/u/My#Disks/game.dsk";   // '#' mid-value
    const std::string kLeadHash = "#literal-hash-start";        // '#' at value start
    const std::string kNewline = "line1\nline2\nline3";         // embedded newlines
    const std::string kBack    = "weird\\path\\name";           // backslashes
    const std::string kPlain   = "PlainValue123";

    {
        pom2::Settings s;
        s.setString("disk_path", kHash);
        s.setString("lead",      kLeadHash);
        s.setString("note",      kNewline);
        s.setString("back",      kBack);
        s.setString("plain",     kPlain);
        s.setInt   ("num",       4242);
        s.setBool  ("flag",      true);
        assert(s.save());
    }

    {
        pom2::Settings s;
        assert(s.load());
        assert(s.getString("disk_path") == kHash   && "'#' mid-value must survive");
        assert(s.getString("lead")      == kLeadHash&& "'#'-leading value must survive");
        assert(s.getString("note")      == kNewline && "embedded newlines must survive");
        assert(s.getString("back")      == kBack    && "backslashes must survive");
        assert(s.getString("plain")     == kPlain);
        // Sanity: the ints/bools still parse.
        assert(s.getString("num")  == "4242");
        assert(s.getString("flag") == "true" || s.getString("flag") == "1");
    }

    // ── Boundary whitespace: load() trims, so only escaping saves these ──
    {
        const std::vector<std::string> ws = {
            " ", "  ", "\t", " \t ", " lead", "trail ", " both ", "  two  ",
            "\tleadtab", "trailtab\t",
        };
        pom2::Settings w;
        for (size_t i = 0; i < ws.size(); ++i) w.setString("w" + std::to_string(i), ws[i]);
        assert(w.save());
        pom2::Settings r;
        assert(r.load());
        for (size_t i = 0; i < ws.size(); ++i) {
            const std::string k = "w" + std::to_string(i);
            if (r.getString(k, "<<missing>>") != ws[i]) {
                std::printf("FAIL: boundary-whitespace value %s did not round-trip "
                            "(want %zu bytes, got %zu)\n", k.c_str(), ws[i].size(),
                            r.getString(k, "").size());
                return 1;
            }
        }
    }

    // ── Typed accessors, floats especially ──────────────────────────────
    {
        const std::vector<int> ints = { 0, 1, -1, 127, -128, 32767, -32768,
                                        2147483647, -2147483647 };
        // 1/3 and 2/3 are the cases the 6-digit default got wrong; the rest
        // cover the exponent forms and everyday slider values.
        const std::vector<float> floats = { 0.0f, 1.0f, -1.0f, 0.5f, 0.75f,
                                            1.0f / 3.0f, 2.0f / 3.0f,
                                            1e-6f, 1e6f, -2.5e-3f, 0.1f, 0.2f };
        pom2::Settings s;
        for (size_t i = 0; i < ints.size(); ++i)   s.setInt  ("i" + std::to_string(i), ints[i]);
        for (size_t i = 0; i < floats.size(); ++i) s.setFloat("f" + std::to_string(i), floats[i]);
        s.setBool("bt", true);
        s.setBool("bf", false);
        assert(s.save());

        pom2::Settings r;
        assert(r.load());
        int bad = 0;
        for (size_t i = 0; i < ints.size(); ++i) {
            const std::string k = "i" + std::to_string(i);
            const int got = r.getInt(k, -999999);
            if (got != ints[i]) {
                std::printf("FAIL: int %s want %d got %d\n", k.c_str(), ints[i], got);
                ++bad;
            }
        }
        for (size_t i = 0; i < floats.size(); ++i) {
            const std::string k = "f" + std::to_string(i);
            const float got = r.getFloat(k, -1e30f);
            if (got != floats[i]) {
                std::printf("FAIL: float %s want %.9g got %.9g (stored \"%s\")\n",
                            k.c_str(), double(floats[i]), double(got),
                            r.getString(k, "?").c_str());
                ++bad;
            }
        }
        if (r.getBool("bt", false) != true || r.getBool("bf", true) != false) {
            std::printf("FAIL: bool round-trip\n");
            ++bad;
        }
        if (bad) { fs::remove_all(home); return 1; }
    }

    // A damaged or hostile line-oriented state file must be rejected before
    // getline can grow a multi-megabyte string during application startup.
    {
        pom2::Settings pathProbe;
        std::ofstream huge(pathProbe.getStorePath(),
                           std::ios::binary | std::ios::trunc);
        assert(huge);
        huge.seekp(4 * 1024 * 1024);
        huge.put('x');
        huge.close();
        pom2::Settings rejected;
        assert(!rejected.load());
    }

    fs::remove_all(home);
    std::printf("OK settings_roundtrip (#-in-value, newline, backslash, "
                "boundary whitespace, int/float/bool round-trip)\n");
    return 0;
}
