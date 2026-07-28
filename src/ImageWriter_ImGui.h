// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriter_ImGui — the paper tray: shows what the emulated Apple
// ImageWriter II has printed. Front-panel buttons (FORM FEED / RESET),
// the completed-sheet stack, a zoomable page view and PNG export.
//
// Unlike the other *_ImGui panels this one is not purely data-in /
// actions-out: it owns a GL texture (the page raster is a megapixel image,
// so it is uploaded once per change rather than rebuilt by the caller) and
// it drives the `ImageWriter` directly. That is safe because an
// `ImageWriter` is host-side only — no emulator state, no `stateMutex`,
// UI thread throughout. MainWindow still owns the printer itself and is
// the one that feeds it bytes drained from the interface card's spool.

#ifndef POM2_IMAGEWRITER_IMGUI_H
#define POM2_IMAGEWRITER_IMGUI_H

#include "ImageWriter.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class ImageWriter_ImGui
{
public:
    struct HostInfo {
        /// Where the bytes come from, e.g. "Printer card (slot 1)".
        std::string sourceLabel;
        bool        haveSource   = false;
        /// False on the browser build — nothing written to MEMFS survives.
        bool        canSaveFiles = true;
        /// Directory PNG exports land in (created on demand).
        std::string saveDir      = "printouts";

        /// Interface-card DIP block, when the plugged card has one (the
        /// Grappler+'s S1 printer-type switches). The panel is the natural
        /// home for it: what the card *says* it is talking to decides
        /// which dialect of escape codes reaches this printer, and a
        /// mismatch is exactly what makes a printout come out as noise.
        /// Empty options list = no DIP UI for this card.
        struct DipOption { const char* label; int value; };
        std::vector<DipOption>    cardDipOptions;
        int                       cardDipValue = 0;
        std::function<void(int)>  onCardDipChanged;
        /// Value the card should be on to speak this printer's language.
        int                       cardDipRecommended = -1;

        /// Whether a full printer buffer blocks the guest (real ACK
        /// handshake). Host-owned because it lives on the card, not the
        /// printer; surfaced here because this is where its effect shows.
        bool                      backPressure = false;
        std::function<void(bool)> onBackPressureChanged;
    };

    ImageWriter_ImGui() = default;
    ~ImageWriter_ImGui();

    ImageWriter_ImGui(const ImageWriter_ImGui&)            = delete;
    ImageWriter_ImGui& operator=(const ImageWriter_ImGui&) = delete;

    void render(bool* open, ImageWriter& iw, const HostInfo& host);

    /// Drop the GL texture (call before the GL context goes away).
    void shutdown();

private:
    unsigned int tex_  = 0;
    int          texW_ = 0, texH_ = 0;
    /// What the texture currently holds: page index (-1 = the sheet in
    /// progress) + the raster revision it was built from.
    int          texPage_ = -2;
    uint32_t     texRev_  = 0;

    int   viewPage_ = -1;        // -1 = sheet in progress
    bool  follow_   = true;      // snap to the newest sheet
    int   zoomMode_ = 0;         // 0 = fit width, else index into kZooms
    std::vector<uint8_t> rgba_;  // scratch, reused across uploads
    std::string status_;

    void uploadPage(const ImageWriter::Page& p, int pageIdx, uint32_t rev);
    bool savePagePng(const ImageWriter::Page& p, const std::string& path,
                     std::string& err);
};

} // namespace pom2

#endif // POM2_IMAGEWRITER_IMGUI_H
