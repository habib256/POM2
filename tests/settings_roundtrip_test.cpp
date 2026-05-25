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
// Drives save()→load() through a real file by pointing HOME at a temp dir.

#include "Settings.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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

    fs::remove_all(home);
    std::printf("OK settings_roundtrip (#-in-value, newline, backslash round-trip)\n");
    return 0;
}
