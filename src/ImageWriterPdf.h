// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriterPdf — multi-page PDF export of ImageWriter sheets.
//
// One concern: serialise a stack of `ImageWriter::Page` rasters into a
// single self-contained PDF file, one PDF page per sheet, each at its
// true physical size (`w/dpi` x `h/dpi` inches → PDF points).
//
// Why PDF-from-raster and not PostScript: the reference implementation
// (greg-kennedy/ImageWriter) emits PostScript with an ASCII85 image per
// page, but a bare .ps is a dead end on most modern hosts while every
// viewer opens PDF. The cost difference is small once the page raster
// exists — a PDF is the same "one image per page" idea plus an xref
// table.
//
// Encoding: each sheet is embedded as an 8-bit /Indexed /DeviceRGB image
// — the ImageWriter raster is already exactly that (one byte per pixel in
// the `yyyxxxxx` ribbon/intensity encoding, palette recovered via
// `ImageWriter::indexToRgb`) — compressed with /FlateDecode using stb's
// `stbi_zlib_compress` (already in-repo for PNG export; a zlib stream is
// what FlateDecode consumes). No new dependency, ~6x smaller than the
// equivalent RGB and ~30x smaller than uncompressed.

#ifndef POM2_IMAGEWRITER_PDF_H
#define POM2_IMAGEWRITER_PDF_H

#include "ImageWriter.h"

#include <string>
#include <vector>

namespace pom2 {

/// Write `pages` (in order, one PDF page each) to `path`. Parent
/// directories are created. Returns false and fills `err` on failure;
/// an empty `pages` is an error (a zero-page PDF is malformed).
/// Pointers must be non-null and outlive the call.
bool writeImageWriterPdf(const std::vector<const ImageWriter::Page*>& pages,
                         const std::string& path, std::string& err);

} // namespace pom2

#endif // POM2_IMAGEWRITER_PDF_H
