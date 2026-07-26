// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ImageWriter_ImGui — see the header for why this panel owns GL state.

#include "ImageWriter_ImGui.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// Declarations only — the single non-static stb_image_write implementation
// lives in Pom2HgrPaintHost.cpp (see the note at the top of that file).
#include "stb_image_write.h"

namespace pom2 {

namespace {

constexpr float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 2.0f };
constexpr const char* kZoomLabels =
    "Fit width\0" "25%\0" "50%\0" "75%\0" "100%\0" "200%\0";

// Page DPI choices. 144 is the reference's default; 72/96 are cheap
// previews, 216/288 are 3x/4x the 72 dpi pin pitch for print-quality PNGs.
constexpr int kDpis[] = { 72, 96, 120, 144, 216, 288 };
constexpr const char* kDpiLabels =
    "72 dpi\0" "96 dpi\0" "120 dpi\0" "144 dpi\0" "216 dpi\0" "288 dpi\0";

std::string timestampedName(const char* prefix, const char* ext)
{
    const auto  t  = std::time(nullptr);
    const auto  tm = *std::localtime(&t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return std::string(prefix) + stamp + ext;
}

} // namespace

ImageWriter_ImGui::~ImageWriter_ImGui() { shutdown(); }

void ImageWriter_ImGui::shutdown()
{
    if (tex_) {
        GLuint t = tex_;
        glDeleteTextures(1, &t);
        tex_ = 0;
    }
    texPage_ = -2;
    texRev_  = 0;
    texW_ = texH_ = 0;
}

void ImageWriter_ImGui::uploadPage(const ImageWriter::Page& p,
                                   int pageIdx, uint32_t rev)
{
    if (p.w <= 0 || p.h <= 0) return;
    // Completed sheets never change, so their revision is irrelevant; the
    // sheet in progress re-uploads whenever the printer touched a dot.
    if (tex_ && texPage_ == pageIdx && (pageIdx >= 0 || texRev_ == rev)) return;

    ImageWriter::pageToRgba(p, rgba_);

    if (!tex_) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_  = t;
        texW_ = texH_ = 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (p.w != texW_ || p.h != texH_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.w, p.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
        texW_ = p.w;
        texH_ = p.h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p.w, p.h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
    }
    texPage_ = pageIdx;
    texRev_  = rev;
}

bool ImageWriter_ImGui::savePagePng(const ImageWriter::Page& p,
                                    const std::string& path, std::string& err)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path out(path);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);

    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(p, rgba);
    if (stbi_write_png(path.c_str(), p.w, p.h, 4, rgba.data(), p.w * 4) == 0) {
        err = "stbi_write_png failed (is " + out.string() + " writable?)";
        return false;
    }
    return true;
}

void ImageWriter_ImGui::render(bool* open, ImageWriter& iw,
                               const HostInfo& host)
{
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_PRINT " ImageWriter II###imageWriterPanel",
                      open)) {
        ImGui::End();
        return;
    }

    const size_t nDone = iw.completedPageCount();
    const int    nTotal = static_cast<int>(nDone) + 1;   // + sheet in progress

    // ─── Front panel ─────────────────────────────────────────────────────
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT " Form feed")) {
        iw.formFeed();
        follow_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Eject the sheet in progress onto the stack.\n"
                          "A blank sheet is not ejected (like the real button).");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset printer")) {
        iw.resetPrinterHard();
        status_ = "Printer reset — pitch, margins and soft switches back to defaults.";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Power-cycle the printer: factory settings, current "
                          "sheet discarded. Ejected sheets are kept.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear all")) {
        iw.clearAll();
        viewPage_ = -1;
        texPage_  = -2;
        status_   = "Paper tray emptied.";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", host.haveSource
                        ? host.sourceLabel.c_str()
                        : "no printer interface card plugged");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The ImageWriter is the printer, not the card. "
                          "Plug a Printer or Grappler+ card in Slot Config, "
                          "then PR#n from BASIC.");

    // ─── Page selector ───────────────────────────────────────────────────
    if (follow_) viewPage_ = -1;
    int shown = (viewPage_ < 0) ? static_cast<int>(nDone) : viewPage_;
    shown = std::clamp(shown, 0, nTotal - 1);

    ImGui::BeginDisabled(shown <= 0);
    if (ImGui::Button(ICON_FA_ARROW_LEFT "##pgPrev")) {
        viewPage_ = shown - 1;
        follow_   = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(shown >= nTotal - 1);
    if (ImGui::Button(ICON_FA_ARROW_RIGHT "##pgNext")) {
        viewPage_ = (shown + 1 >= nTotal - 1) ? -1 : shown + 1;
        follow_   = (shown + 1 >= nTotal - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Sheet %d / %d%s", shown + 1, nTotal,
                (shown == nTotal - 1) ? "  (in the printer)" : "");
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow", &follow_) && follow_) viewPage_ = -1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Always show the sheet currently under the head.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##zoom", &zoomMode_, kZoomLabels);

    if (iw.droppedPageCount() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu older sheet%s dropped)",
                            iw.droppedPageCount(),
                            iw.droppedPageCount() == 1 ? "" : "s");
    }

    // ─── Printer settings ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Printer settings")) {
        int paperIdx = static_cast<int>(iw.paperSize());
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("Paper", ImageWriter::paperSizeName(
                                           iw.paperSize()))) {
            for (int i = 0; i < static_cast<int>(
                                    ImageWriter::PaperSize::Count); ++i) {
                const auto s = static_cast<ImageWriter::PaperSize>(i);
                if (ImGui::Selectable(ImageWriter::paperSizeName(s),
                                      i == paperIdx)) {
                    iw.setPaperSize(s);
                    texPage_ = -2;
                }
            }
            ImGui::EndCombo();
        }

        int dpiIdx = 3;
        for (int i = 0; i < static_cast<int>(sizeof(kDpis) / sizeof(kDpis[0])); ++i)
            if (kDpis[i] == iw.dpi()) dpiIdx = i;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("Page resolution", &dpiIdx, kDpiLabels)) {
            iw.setDpi(kDpis[dpiIdx]);
            texPage_ = -2;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Raster density of the rendered page. The "
                              "printer's own dot densities (72-320 dpi) are "
                              "set by the guest and unaffected.\n"
                              "Changing this restarts the sheet in progress.");

        bool af = iw.autoFeed();
        if (ImGui::Checkbox("Auto line-feed after CR", &af)) iw.setAutoFeed(af);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turn on when printouts come out on a single "
                              "line; turn off when everything double-spaces.");
    }

    // ─── Save ────────────────────────────────────────────────────────────
    const ImageWriter::Page& page =
        (shown < static_cast<int>(nDone)) ? iw.completedPage(
                                                static_cast<size_t>(shown))
                                          : iw.currentPage();

    if (!host.canSaveFiles) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_FLOPPY_DISK " Save sheet as PNG");
        ImGui::SameLine();
        ImGui::Button("Save all sheets");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(unavailable in the browser build)");
    } else {
        namespace fs = std::filesystem;
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save sheet as PNG")) {
            const std::string path =
                (fs::path(host.saveDir) /
                 timestampedName("imagewriter-", ".png")).string();
            std::string err;
            status_ = savePagePng(page, path, err)
                    ? "Saved " + path
                    : "Save failed: " + err;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(nDone == 0);
        if (ImGui::Button("Save all sheets")) {
            const std::string base = timestampedName("imagewriter-", "");
            size_t ok = 0;
            std::string err;
            for (size_t i = 0; i < nDone; ++i) {
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "-p%02zu.png", i + 1);
                const std::string path =
                    (fs::path(host.saveDir) / (base + suffix)).string();
                if (savePagePng(iw.completedPage(i), path, err)) ++ok;
            }
            status_ = "Saved " + std::to_string(ok) + " / " +
                      std::to_string(nDone) + " sheet(s) → " + host.saveDir +
                      (ok == nDone ? "" : ("  (" + err + ")"));
        }
        ImGui::EndDisabled();
    }

    // ─── Status line ─────────────────────────────────────────────────────
    const ImageWriter::Status st = iw.status();
    ImGui::Separator();
    ImGui::TextDisabled(
        "%llu byte%s  |  head %.2f\" x %.2f\"  |  %.1f cpi  |  %d dpi gfx"
        "  |  ribbon %s  |  %s%s",
        static_cast<unsigned long long>(iw.bytesReceived()),
        iw.bytesReceived() == 1 ? "" : "s",
        st.headX, st.headY, st.cpi, st.graphicsDpi, st.colorName,
        st.styleText.c_str(), st.inGraphics ? "  |  bit-image" : "");
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // ─── Page view ───────────────────────────────────────────────────────
    uploadPage(page, (shown < static_cast<int>(nDone)) ? shown : -1,
               iw.revision());

    ImGui::BeginChild("##iwPaper", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (iw.bytesReceived() == 0 && nDone == 0) {
        ImGui::TextDisabled(
            "Nothing printed yet. Plug a Printer or Grappler+ card in\n"
            "Slot Config, then from BASIC:   PR#1 : PRINT \"HELLO\" : PR#0");
        ImGui::Separator();
    }
    if (tex_ && texW_ > 0) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = (zoomMode_ == 0)
                          ? std::max(0.05f, avail / static_cast<float>(texW_))
                          : kZooms[zoomMode_ - 1];
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex_)),
                     ImVec2(texW_ * scale, texH_ * scale));
    } else {
        ImGui::TextDisabled("(no page)");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace pom2
