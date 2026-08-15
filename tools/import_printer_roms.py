#!/usr/bin/env python3
"""Transcode web-a2e's ImageWriter character-ROM tables into a C++ header.

POM2 vendors the GENERATED header (src/ImageWriterRom.h), not the JavaScript.
This script is kept in-repo for two reasons: it is the provenance record, and
re-running it is how a future locale or a second printer's bank gets added.

    tools/import_printer_roms.py <path-to-web-a2e> > src/ImageWriterRom.h

── Provenance ──────────────────────────────────────────────────────────────

Source project: https://github.com/mikedaley/web-a2e  (MIT, (c) 2025 Mike Daley)
Source data:    Apple ImageWriter II / ImageWriter Technical Reference Manuals,
                Appendix C — the dot patterns Apple PUBLISHED for these faces,
                transcribed by that project's rom-editor.html.

These are therefore not chip dumps. Mike Daley's MIT grant covers the
transcription; the underlying typeface design is Apple's, published in the
printer's own technical reference. POM2 records this the same way it records
the AppleWin SSI263 phoneme blob — visibly, in the file header and in
docs/lle_vs_hle.md — rather than letting it pass as clean-room work.

Decision of 2026-08-10: POM2 keeps these tables under web-a2e's MIT grant.

── Data shapes in the source ───────────────────────────────────────────────

    draft        12 columns, bit 0 = wire 1 … bit 8 = wire 9  (values > 0xFF)
    nlq fixed    16 columns, up to 18 bits per column
    nlq prop     variable columns (~6-19), up to 18 bits
    iw1 std      as above for the ImageWriter I banks

Everything is normalised here to ONE shape — up to 18 columns, 32-bit column
values, plus a significant-width byte — because the renderer wants one code
path and 40 KB of padding is not worth a second one.
"""

import re
import sys
import os

# (export symbol, C++ name, human description)
BANKS = [
    ("imagewriter-ii-rom-draft.js",       "IW2_DRAFT_ROM",
     "kIw2Draft",     "ImageWriter II draft, 12 columns x 9 wires"),
    ("imagewriter-ii-rom-nlq-fixed.js",   "IW2_NLQ_FIXED",
     "kIw2NlqFixed",  "ImageWriter II NLQ fixed, 16 columns x 18 rows"),
    ("imagewriter-ii-rom-nlq-prop.js",    "IW2_NLQ_PROP_ROM",
     "kIw2NlqProp",   "ImageWriter II NLQ proportional, variable width"),
    ("imagewriter-ii-rom-standard-fixed.js", "IW2_STANDARD_FIXED",
     "kIw2StdFixed",  "ImageWriter II correspondence fixed (the default face)"),
    ("imagewriter-ii-rom-standard-prop.js",  "IW2_STANDARD_PROP",
     "kIw2StdProp",   "ImageWriter II correspondence proportional"),
    ("imagewriter-i-rom-standard-fixed.js", "IW1_STANDARD_FIXED",
     "kIw1Fixed",     "ImageWriter I standard fixed"),
    ("imagewriter-i-rom-standard-prop.js",  "IW1_STANDARD_PROP",
     "kIw1Prop",      "ImageWriter I standard proportional"),
    ("apple-dmp-rom.js", "DMP_STANDARD_FIXED",
     "kDmpFixed",     "Apple DMP (C. Itoh 8510) standard fixed"),
    ("apple-dmp-rom.js", "DMP_STANDARD_PROP",
     "kDmpProp",      "Apple DMP standard proportional"),
    ("epson-fx80-rom.js", "EPSON_FX_ROM",
     "kEpsonFx",      "Epson FX-80 Roman, 12 columns x 9 wires"),
]

LOCALES = ["UK", "FR", "DE", "IT", "SE", "ES", "DK"]

FIRST_CODE = 0x20
LAST_CODE = 0x7E
CODE_COUNT = LAST_CODE - FIRST_CODE + 1
MAX_COLS = 20


def strip_comments(text):
    """Remove // line comments and /* */ blocks.

    NOT cosmetic. Each ROM row is annotated with the character it draws —
    `0x7d: [...], // }` — and the brace-walk below counts those as real
    braces, which truncated every table at the '}' row and silently dropped
    the last glyph ('~', $7E). The symptom was seven banks each missing
    exactly one code.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def read_object(text, symbol):
    """Return {code: [values]} for `export const <symbol> = { ... }`."""
    text = strip_comments(text)
    m = re.search(r"export\s+const\s+" + re.escape(symbol) + r"\s*=\s*\{", text)
    if not m:
        return {}
    # Walk braces so a nested array or comment cannot end the object early.
    i = m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                body = text[i + 1:j]
                break
    else:
        raise SystemExit("unterminated object for " + symbol)

    out = {}
    for code_s, vals_s in re.findall(r"(0x[0-9a-fA-F]+)\s*:\s*\[([^\]]*)\]", body):
        vals = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", vals_s)]
        out[int(code_s, 0)] = vals
    return out


def significant_width(cols):
    """Stored width = escapement. The source keeps the trailing blank spacer
    column in the data, and that spacer IS part of the advance, so the width
    is simply the stored length — NOT the last inked column. Trimming it would
    make every proportional glyph touch its neighbour."""
    return len(cols)


def emit_glyph(cols):
    padded = list(cols) + [0] * (MAX_COLS - len(cols))
    body = ", ".join("0x%05x" % c for c in padded)
    return "{ { %s }, %d }" % (body, significant_width(cols))


def comment_char(code):
    """The character to show in the row comment — or a placeholder.

    $5C is a BACKSLASH, and a backslash at the end of a C++ `//` comment
    CONTINUES THAT COMMENT ONTO THE NEXT LINE. Emitting it raw swallowed the
    row after it, so every generated bank silently held 94 initialisers for a
    95-element array and the last glyph ('~') came out blank. The array is
    fixed-size, so there is no "too few initializers" diagnostic — it compiles
    clean and prints a hole. Anything that would end the line in a backslash
    gets a placeholder instead.
    """
    if code == 0x5C:
        return "(backslash)"
    if 0x20 < code < 0x7F:
        return chr(code)
    return " "


def emit_bank(name, desc, table):
    lines = []
    lines.append("/// %s." % desc)
    lines.append("inline constexpr IwGlyph %s[kIwCodeCount] = {" % name)
    for code in range(FIRST_CODE, LAST_CODE + 1):
        cols = table.get(code)
        ch = comment_char(code)
        if cols is None:
            lines.append("    { {}, 0 },%s// $%02X" % (" " * 4, code))
        else:
            if len(cols) > MAX_COLS:
                raise SystemExit("glyph $%02X in %s has %d columns > %d"
                                 % (code, name, len(cols), MAX_COLS))
            lines.append("    %s,  // $%02X %s" % (emit_glyph(cols), code, ch))
    lines.append("};")
    return "\n".join(lines)


def emit_overrides(name, per_locale):
    rows = []
    for li, loc in enumerate(LOCALES):
        table = per_locale.get(loc, {})
        for code in sorted(table):
            rows.append("    { IwLocale::%s, 0x%02X, %s },"
                        % (loc, code, emit_glyph(table[code])))
    if not rows:
        # ONE value-initialised placeholder, not `[] = {}`. A zero-length array
        # is a GCC/Clang extension that MSVC rejects outright (C2466: "cannot
        # allocate an array of constant size 0"), which broke the Windows
        # release build. Consumers iterate to `...Count`, which stays 0, so the
        # placeholder is never read.
        return ("inline constexpr IwOverride %s[1] = {};\n"
                "inline constexpr size_t %sCount = 0;" % (name, name))
    return ("inline constexpr IwOverride %s[] = {\n%s\n};\n"
            "inline constexpr size_t %sCount =\n"
            "    sizeof(%s) / sizeof(%s[0]);"
            % (name, "\n".join(rows), name, name, name))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.join(sys.argv[1], "src", "js", "printer")
    if not os.path.isdir(root):
        # Also accept a directory of the raw .js files.
        root = sys.argv[1]

    out = []
    out.append("""// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriterRom.h — GENERATED, DO NOT EDIT BY HAND.
//   Regenerate with: tools/import_printer_roms.py <path-to-web-a2e>
//
// Dot-matrix character ROM data for the Apple ImageWriter I and II.
//
// ── Provenance ────────────────────────────────────────────────────────────
//
// Transcoded from https://github.com/mikedaley/web-a2e (MIT, (c) 2025 Mike
// Daley), whose tables were in turn transcribed from the dot patterns Apple
// PUBLISHED in the ImageWriter / ImageWriter II Technical Reference Manuals,
// Appendix C.
//
// These are therefore NOT chip dumps. The MIT grant covers the transcription;
// the typeface design underneath is Apple's, published by Apple in the
// printer's own technical reference. POM2 states this rather than letting it
// pass as clean-room work — the same treatment as the AppleWin SSI263 phoneme
// blob. See docs/lle_vs_hle.md and docs/printer_plan.md § 3.
//
// ── Format ────────────────────────────────────────────────────────────────
//
// One value per dot column, left to right. Bit 0 is the TOP wire; the draft
// banks reach bit 8 (wire 9, the descender/underline row) and the NLQ banks
// reach bit 17 (row 18), which is why the column type is 32-bit rather than
// the byte the paper tables suggest.
//
// `width` is the glyph's ESCAPEMENT in columns, not its inked extent: the
// source keeps the trailing blank spacer column, and that spacer is part of
// the advance. Trimming it would make every proportional glyph touch the
// next one.
//
// Codes run $20-$7E. A glyph the bank does not carry has width 0, and the
// renderer falls back to POM2's bundled CP437 font for it.

#ifndef POM2_IMAGEWRITER_ROM_H
#define POM2_IMAGEWRITER_ROM_H

#include <cstddef>
#include <cstdint>

namespace pom2 {
namespace iwrom {

inline constexpr uint8_t kIwFirstCode = 0x%02X;
inline constexpr uint8_t kIwLastCode  = 0x%02X;
inline constexpr size_t  kIwCodeCount = %d;
inline constexpr size_t  kIwMaxCols   = %d;

/// The seven alternate-language sets the DIP switches / `ESC D` select. `US`
/// is the base bank, so it never appears in an override table.
enum class IwLocale : uint8_t { US = 0, UK, FR, DE, IT, SE, ES, DK, Count };

struct IwGlyph {
    uint32_t cols[kIwMaxCols];
    uint8_t  width;          ///< escapement in columns; 0 = not in this bank
};

struct IwOverride {
    IwLocale locale;
    uint8_t  code;
    IwGlyph  glyph;
};

/// Locale-substituted glyph for `code`, or nullptr when the base bank's
/// glyph stands. Ten code points at most per locale, so a linear scan is
/// cheaper than any index.
inline const IwGlyph* findOverride(const IwOverride* table, size_t count,
                                   IwLocale locale, uint8_t code)
{
    if (locale == IwLocale::US) return nullptr;
    for (size_t i = 0; i < count; ++i)
        if (table[i].locale == locale && table[i].code == code)
            return &table[i].glyph;
    return nullptr;
}
""" % (FIRST_CODE, LAST_CODE, CODE_COUNT, MAX_COLS))

    for filename, symbol, cname, desc in BANKS:
        path = os.path.join(root, filename)
        if not os.path.isfile(path):
            sys.stderr.write("missing %s — skipped\n" % path)
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        base = read_object(text, symbol)
        if not base:
            sys.stderr.write("no data for %s in %s\n" % (symbol, filename))
            continue
        out.append("\n// " + "─" * 70)
        out.append(emit_bank(cname, desc, base))

        per_locale = {}
        for loc in LOCALES:
            t = read_object(text, symbol + "_" + loc)
            if t:
                per_locale[loc] = t
        out.append(emit_overrides(cname + "Overrides", per_locale))

    out.append("""
} // namespace iwrom
} // namespace pom2

#endif // POM2_IMAGEWRITER_ROM_H""")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
