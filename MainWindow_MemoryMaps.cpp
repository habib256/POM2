// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MainWindow_MemoryMaps.cpp — Memory map visualisation widgets ported from
// POM1's `MainWindow_DebugWindows.cpp`. Three companion views over the same
// 64 KB Apple II address space:
//
//   • Memory Map Bar              — tall, twin-bar, distorted-scale layout
//                                    with a label gutter between the bars,
//                                    PC arrow, and viewport overlay.
//   • Memory Map Bar (Horizontal) — wide / short variant of the same data
//                                    with an inline single bar, top axis,
//                                    PC triangle below.
//   • Memory Map Grid             — 16 × 16 page grid (1 cell = 256 B)
//                                    with PC + SP indicators, region
//                                    legend, and the canonical Apple II
//                                    I/O reference panel underneath.
//
// All three share `buildMemoryRegions()`, which lays out the canonical
// Apple II memory map (zero page, stack, text/HGR pages, soft switches,
// slot ROMs, Applesoft, Monitor) plus the Disk II PROM band when slot 6
// is plugged. Colours mirror `MemoryViewer_ImGui::regionColour()` so the
// bar, the grid, and the hex viewer feel like one tool.
//
// Click anywhere on a bar/grid cell to centre the Memory viewer on that
// address. The viewer's currently-visible range is overlaid on the bars
// as a white bracket so you can see what slice of memory is in focus.

#include "MainWindow.h"

#include "DiskIICard.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryViewer_ImGui.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// Apple II canonical region palette. Hues match
// `MemoryViewer_ImGui::regionColour()` so the bar, grid and hex viewer all
// agree on what each zone looks like at a glance.
constexpr ImU32 kRamColor   = IM_COL32( 80, 200,  80, 255);  // user RAM (heat-mapped at render)
constexpr ImU32 kZpColor    = IM_COL32(242, 217, 128, 255);  // zero page (sand)
constexpr ImU32 kStackColor = IM_COL32(255, 166, 140, 255);  // stack (coral)
constexpr ImU32 kTextColor  = IM_COL32(166, 255, 191, 255);  // text page (mint)
constexpr ImU32 kHGRColor   = IM_COL32(140, 217, 255, 255);  // HGR page (sky)
constexpr ImU32 kIOColor    = IM_COL32(255, 140, 242, 255);  // soft switches (pink)
constexpr ImU32 kSlotColor  = IM_COL32(191, 166, 255, 255);  // slot ROMs (lavender)
constexpr ImU32 kDiskColor  = IM_COL32(225, 145, 255, 255);  // Disk II P5A PROM
constexpr ImU32 kRomColor   = IM_COL32(255, 217, 115, 255);  // Applesoft / Monitor

inline ImU32 lighten(ImU32 c, int amount)
{
    int r = std::min(255, (int)((c >>  0) & 0xFF) + amount);
    int g = std::min(255, (int)((c >>  8) & 0xFF) + amount);
    int b = std::min(255, (int)((c >> 16) & 0xFF) + amount);
    int a = (c >> 24) & 0xFF;
    return IM_COL32(r, g, b, a);
}
inline ImU32 darken(ImU32 c, int amount)
{
    int r = std::max(0, (int)((c >>  0) & 0xFF) - amount);
    int g = std::max(0, (int)((c >>  8) & 0xFF) - amount);
    int b = std::max(0, (int)((c >> 16) & 0xFF) - amount);
    int a = (c >> 24) & 0xFF;
    return IM_COL32(r, g, b, a);
}

// Per-page snapshot used by the bar variants: last-match-wins region
// resolution + heatmap pass over the User-RAM bands.
struct PageInfo { ImU32 color; const char* label; };

inline void buildPageMap(const std::vector<MainWindow::MemRegion>& regions,
                         const uint8_t* memPtr,
                         PageInfo (&pageMap)[256])
{
    const int numRegions = static_cast<int>(regions.size());
    for (int page = 0; page < 256; ++page) {
        const uint16_t addr = static_cast<uint16_t>(page << 8);
        ImU32 color = IM_COL32(40, 40, 40, 255);
        const char* label = "Unmapped";
        for (int r = 0; r < numRegions; ++r) {
            if (addr >= regions[r].start && addr <= regions[r].end) {
                color = regions[r].color;
                label = regions[r].label;
            }
        }
        // Heatmap only on the generic User-RAM regions — text / HGR / I/O
        // / ROM keep their distinct hue so the structural map stays
        // readable at a glance.
        if (color == kRamColor) {
            bool hasData = false;
            for (int b = 0; b < 256; ++b) {
                if (memPtr[addr + b] != 0) { hasData = true; break; }
            }
            color = hasData ? IM_COL32( 80, 220,  80, 255)
                            : IM_COL32( 20,  60,  20, 255);
        }
        pageMap[page] = { color, label };
    }
}

struct FlatRegion {
    uint16_t start, end;  // inclusive
    ImU32 color;
    const char* label;
};

inline std::vector<FlatRegion> flattenPageMap(const PageInfo (&pageMap)[256])
{
    std::vector<FlatRegion> flat;
    flat.reserve(32);
    flat.push_back({ 0x0000, 0x00FF, pageMap[0].color, pageMap[0].label });
    for (int page = 1; page < 256; ++page) {
        auto& prev = flat.back();
        if (pageMap[page].color == prev.color && pageMap[page].label == prev.label) {
            prev.end = static_cast<uint16_t>((page << 8) | 0xFF);
        } else {
            flat.push_back({
                static_cast<uint16_t>(page << 8),
                static_cast<uint16_t>((page << 8) | 0xFF),
                pageMap[page].color,
                pageMap[page].label
            });
        }
    }
    return flat;
}

} // namespace

// ─── Region builder ───────────────────────────────────────────────────────

std::vector<MainWindow::MemRegion> MainWindow::buildMemoryRegions()
{
    std::vector<MemRegion> regions;

    // Layer 0 — user RAM blanket. Heatmap is applied later by the bar
    // renderers (only where this exact colour shines through).
    regions.push_back({ 0x0000, 0xFFFF, kRamColor, "User RAM" });

    // Layer 1 — CPU-reserved structures.
    regions.push_back({ 0x0000, 0x00FF, kZpColor,    "Zero page" });
    regions.push_back({ 0x0100, 0x01FF, kStackColor, "Stack" });

    // Layer 2 — Apple II text / lo-res pages.
    regions.push_back({ 0x0400, 0x07FF, kTextColor, "Text page 1" });
    regions.push_back({ 0x0800, 0x0BFF, kTextColor, "Text page 2" });

    // Layer 3 — HGR pages.
    regions.push_back({ 0x2000, 0x3FFF, kHGRColor, "HGR page 1" });
    regions.push_back({ 0x4000, 0x5FFF, kHGRColor, "HGR page 2" });

    // Layer 4 — soft switches + slot ROMs.
    regions.push_back({ 0xC000, 0xC07F, kIOColor,   "Soft switches / I/O" });
    regions.push_back({ 0xC080, 0xC0FF, kIOColor,   "I/O reserved" });
    regions.push_back({ 0xC100, 0xC7FF, kSlotColor, "Slot ROMs" });
    if (diskCard != nullptr) {
        // Disk II P5A boot PROM lives at slot 6 ($C600-$C6FF) when the
        // card is plugged. Painted on top of the Slot-ROM band so the
        // user sees exactly which slot owns the bytes the boot trick
        // (`JSR $FF58 / TSX / LDA $0100,X`) snags.
        regions.push_back({ 0xC600, 0xC6FF, kDiskColor, "Disk II PROM (slot 6)" });
    }
    regions.push_back({ 0xC800, 0xCFFF, kSlotColor, "Expansion ROM" });

    // Layer 5 — main ROMs.
    regions.push_back({ 0xD000, 0xF7FF, kRomColor, "Applesoft ROM" });
    regions.push_back({ 0xF800, 0xFFFF, kRomColor, "Monitor ROM + vectors" });

    return regions;
}

// ─── Vertical bar (twin-bar with label gutter) ────────────────────────────

void MainWindow::renderMemoryBarWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Map Bar", &showMemoryBar)) {
        ImGui::End();
        return;
    }

    auto regions = buildMemoryRegions();

    // Hold the state mutex across the whole snapshot copy + flattening so
    // the worker can't tear the page heatmap mid-scan.
    std::array<uint8_t, 0x10000> ramSnap;
    uint16_t pc;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        std::memcpy(ramSnap.data(), controller.memory().data(), 0x10000);
        pc = controller.cpu().getProgramCounter();
    }

    PageInfo pageMap[256];
    buildPageMap(regions, ramSnap.data(), pageMap);
    const auto flat = flattenPageMap(pageMap);

    // --- Layout constants ---
    const float gutterW   = 42.0f;
    const float barW      = 48.0f;
    const float bar2W     = 48.0f;
    const float labelGap  = 10.0f;
    const float textH     = ImGui::GetTextLineHeight();
    const float minRegionH = textH + 4.0f;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  barX   = origin.x + gutterW;
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    // Max label width across all flat regions (label + addr sub-line).
    float maxLabelW = 0.0f;
    for (const auto& fr : flat) {
        const uint32_t sizeBytes = static_cast<uint32_t>(fr.end) - fr.start + 1;
        char labelBuf[128];
        if (sizeBytes >= 1024)
            std::snprintf(labelBuf, sizeof(labelBuf), "%s (%u KB)", fr.label, (unsigned)(sizeBytes / 1024));
        else
            std::snprintf(labelBuf, sizeof(labelBuf), "%s (%u B)",  fr.label, (unsigned)sizeBytes);
        float w = ImGui::CalcTextSize(labelBuf).x;
        char addrBuf[24];
        std::snprintf(addrBuf, sizeof(addrBuf), "$%04X-$%04X", fr.start, fr.end);
        w = std::max(w, ImGui::CalcTextSize(addrBuf).x);
        if (w > maxLabelW) maxLabelW = w;
    }
    const float bar2X = barX + barW + labelGap + maxLabelW + labelGap;

    // --- Compute Y positions with minimum height per region ---
    const float availH = std::max(100.0f, ImGui::GetContentRegionAvail().y - 4.0f);
    std::vector<float> regionH(flat.size());
    float totalClamped = 0.0f;
    for (size_t i = 0; i < flat.size(); ++i) {
        float natural = (static_cast<float>(flat[i].end - flat[i].start + 1) / 65536.0f) * availH;
        regionH[i] = std::max(natural, minRegionH);
        totalClamped += regionH[i];
    }
    if (totalClamped > availH) {
        float excess = totalClamped - availH;
        float shrinkable = 0.0f;
        for (size_t i = 0; i < flat.size(); ++i)
            if (regionH[i] > minRegionH) shrinkable += (regionH[i] - minRegionH);
        if (shrinkable > 0.0f) {
            float ratio = std::min(1.0f, excess / shrinkable);
            for (size_t i = 0; i < flat.size(); ++i) {
                if (regionH[i] > minRegionH)
                    regionH[i] -= (regionH[i] - minRegionH) * ratio;
            }
        }
    }
    std::vector<float> regionY0(flat.size()), regionY1(flat.size());
    {
        float curY = origin.y;
        for (size_t i = 0; i < flat.size(); ++i) {
            regionY0[i] = curY;
            regionY1[i] = curY + regionH[i];
            curY += regionH[i];
        }
    }
    const float totalBarH = regionY1.empty() ? 0.0f : (regionY1.back() - origin.y);

    auto addrToY = [&](uint32_t addr) -> float {
        for (size_t i = 0; i < flat.size(); ++i) {
            const uint32_t rStart = flat[i].start;
            const uint32_t rEnd   = static_cast<uint32_t>(flat[i].end) + 1;
            if (addr >= rStart && addr < rEnd) {
                const float t = static_cast<float>(addr - rStart) / static_cast<float>(rEnd - rStart);
                return regionY0[i] + t * (regionY1[i] - regionY0[i]);
            }
        }
        return regionY1.back();
    };

    // --- Outer frames ---
    dl->AddRect(ImVec2(barX  - 1, origin.y - 1),
                ImVec2(barX  + barW  + 1, origin.y + totalBarH + 1),
                IM_COL32(80, 80, 80, 255), 2.0f, 0, 1.0f);
    dl->AddRect(ImVec2(bar2X - 1, origin.y - 1),
                ImVec2(bar2X + bar2W + 1, origin.y + totalBarH + 1),
                IM_COL32(80, 80, 80, 255), 2.0f, 0, 1.0f);

    // --- Pass 1: both bars, mirrored bevels ---
    for (size_t ri = 0; ri < flat.size(); ++ri) {
        const auto& fr = flat[ri];
        const float y0 = regionY0[ri];
        const float y1 = regionY1[ri];
        const float h  = y1 - y0;

        dl->AddRectFilled(ImVec2(barX,  y0), ImVec2(barX  + barW,  y1), fr.color);
        dl->AddRectFilled(ImVec2(bar2X, y0), ImVec2(bar2X + bar2W, y1), fr.color);
        if (h > 4.0f) {
            dl->AddLine(ImVec2(barX,  y0 + 0.5f), ImVec2(barX  + barW,  y0 + 0.5f), lighten(fr.color, 40), 1.0f);
            dl->AddLine(ImVec2(barX,  y1 - 0.5f), ImVec2(barX  + barW,  y1 - 0.5f), darken (fr.color, 40), 1.0f);
            dl->AddLine(ImVec2(bar2X, y0 + 0.5f), ImVec2(bar2X + bar2W, y0 + 0.5f), lighten(fr.color, 40), 1.0f);
            dl->AddLine(ImVec2(bar2X, y1 - 0.5f), ImVec2(bar2X + bar2W, y1 - 0.5f), darken (fr.color, 40), 1.0f);
        }
        dl->AddLine(ImVec2(barX,  y1), ImVec2(barX  + barW,  y1), IM_COL32(30, 30, 30, 200), 1.0f);
        dl->AddLine(ImVec2(bar2X, y1), ImVec2(bar2X + bar2W, y1), IM_COL32(30, 30, 30, 200), 1.0f);
    }

    // --- Pass 2: gap tint + boundary hairlines between the two bars ---
    {
        const float bar1Right = barX + barW;
        const float bar2Left  = bar2X;
        for (size_t ri = 0; ri < flat.size(); ++ri) {
            const ImU32 zoneTint = (flat[ri].color & 0x00FFFFFFu) | (90u << 24);
            dl->AddRectFilled(ImVec2(bar1Right, regionY0[ri]),
                              ImVec2(bar2Left,  regionY1[ri]),
                              zoneTint);
        }
        dl->AddLine(ImVec2(bar1Right, regionY0[0]),
                    ImVec2(bar2Left,  regionY0[0]),
                    IM_COL32(60, 60, 60, 220), 1.5f);
        for (size_t ri = 0; ri < flat.size(); ++ri) {
            dl->AddLine(ImVec2(bar1Right, regionY1[ri]),
                        ImVec2(bar2Left,  regionY1[ri]),
                        IM_COL32(60, 60, 60, 220), 1.5f);
        }
    }

    // --- Pass 3: per-zone labels (drawn last so connectors don't cross them) ---
    for (size_t ri = 0; ri < flat.size(); ++ri) {
        const auto& fr = flat[ri];
        const float y0 = regionY0[ri];
        const float y1 = regionY1[ri];
        const float h  = y1 - y0;

        const uint32_t sizeBytes = static_cast<uint32_t>(fr.end) - fr.start + 1;
        char labelBuf[128];
        if (sizeBytes >= 1024)
            std::snprintf(labelBuf, sizeof(labelBuf), "%s (%u KB)", fr.label, (unsigned)(sizeBytes / 1024));
        else
            std::snprintf(labelBuf, sizeof(labelBuf), "%s (%u B)",  fr.label, (unsigned)sizeBytes);

        const float gapMidX = (barX + barW + bar2X) * 0.5f;
        const float labelW  = ImGui::CalcTextSize(labelBuf).x;
        const float labelX  = gapMidX - labelW * 0.5f;
        const float labelY  = (y0 + y1) * 0.5f - textH * 0.5f;

        dl->AddRectFilled(
            ImVec2(labelX - 3, labelY - 1),
            ImVec2(labelX + labelW + 3, labelY + textH + 1),
            IM_COL32(28, 28, 36, 200), 2.0f);
        dl->AddText(ImVec2(labelX, labelY),
                    IM_COL32(232, 232, 232, 255), labelBuf);

        if (h > textH * 2.0f + 6.0f) {
            char addrBuf[24];
            std::snprintf(addrBuf, sizeof(addrBuf), "$%04X-$%04X", fr.start, fr.end);
            const float addrW = ImGui::CalcTextSize(addrBuf).x;
            const float addrX = gapMidX - addrW * 0.5f;
            const float addrY = labelY + textH + 1;
            dl->AddRectFilled(
                ImVec2(addrX - 3, addrY - 1),
                ImVec2(addrX + addrW + 3, addrY + textH + 1),
                IM_COL32(28, 28, 36, 180), 2.0f);
            dl->AddText(ImVec2(addrX, addrY),
                        IM_COL32(150, 150, 150, 255), addrBuf);
        }
    }

    // --- Address gutter: $0000 top, $FFFF bottom + 8 KB ticks ---
    {
        const float topLabelY = origin.y - 1;
        const float botLabelY = origin.y + totalBarH - textH + 1;
        dl->AddText(ImVec2(origin.x, topLabelY), IM_COL32(180, 180, 180, 255), "$0000");
        dl->AddText(ImVec2(origin.x, botLabelY), IM_COL32(180, 180, 180, 255), "$FFFF");

        const float minSpacing = textH + 2.0f;
        float lastLabelY = topLabelY;
        for (int kb = 8; kb < 64; kb += 8) {
            const uint32_t addr = static_cast<uint32_t>(kb) * 1024;
            const float y = addrToY(addr);
            dl->AddLine(ImVec2(barX, y), ImVec2(barX + barW, y),
                        IM_COL32(255, 255, 255, 20), 1.0f);
            const float labelY = y - textH * 0.5f;
            if (labelY - lastLabelY >= minSpacing && botLabelY - labelY >= minSpacing) {
                char tick[8];
                std::snprintf(tick, sizeof(tick), "$%04X", static_cast<uint16_t>(addr));
                const float tw = ImGui::CalcTextSize(tick).x;
                dl->AddText(ImVec2(barX - tw - 4, labelY),
                            IM_COL32(100, 100, 100, 255), tick);
                lastLabelY = labelY;
            }
        }
    }

    // --- PC indicator on bar 2 ---
    {
        const float pcY = addrToY(pc);
        const float sz  = 6.0f;
        dl->AddTriangleFilled(
            ImVec2(bar2X + bar2W + 3 + sz, pcY - sz),
            ImVec2(bar2X + bar2W + 3,      pcY),
            ImVec2(bar2X + bar2W + 3 + sz, pcY + sz),
            IM_COL32(255, 255, 255, 255));
        dl->AddLine(ImVec2(bar2X, pcY), ImVec2(bar2X + bar2W, pcY),
                    IM_COL32(255, 255, 255, 120), 1.0f);
        char pcLabel[16];
        std::snprintf(pcLabel, sizeof(pcLabel), "PC $%04X", pc);
        const float labelW = ImGui::CalcTextSize(pcLabel).x;
        const float lx = bar2X + bar2W + 5 + sz;
        dl->AddRectFilled(
            ImVec2(lx - 2, pcY - sz - 1),
            ImVec2(lx + labelW + 2, pcY - sz + ImGui::GetTextLineHeight()),
            IM_COL32(40, 40, 50, 220), 2.0f);
        dl->AddText(ImVec2(lx, pcY - sz),
                    IM_COL32(255, 255, 255, 255), pcLabel);
    }

    // --- Memory viewer viewport overlay ---
    if (showMemViewer) {
        const auto vp = memViewer.getViewportRange();
        float vpY0 = addrToY(vp.startAddress);
        float vpY1 = addrToY(vp.endAddress);
        if (vpY1 - vpY0 < 4.0f) vpY1 = vpY0 + 4.0f;
        dl->AddRectFilled(ImVec2(barX, vpY0), ImVec2(barX + barW, vpY1),
                          IM_COL32(255, 255, 255, 35));
        dl->AddRect(ImVec2(barX, vpY0), ImVec2(barX + barW, vpY1),
                    IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
        const float wingW = 4.0f;
        dl->AddLine(ImVec2(barX - wingW, vpY0), ImVec2(barX, vpY0),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(barX - wingW, vpY1), ImVec2(barX, vpY1),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(barX + barW, vpY0), ImVec2(barX + barW + wingW, vpY0),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(barX + barW, vpY1), ImVec2(barX + barW + wingW, vpY1),
                    IM_COL32(255, 255, 255, 200), 1.5f);
    }

    // --- Tooltip + click-to-navigate (bar 1 hot zone) ---
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x >= barX && mouse.x < barX + barW &&
            mouse.y >= origin.y && mouse.y < origin.y + totalBarH) {
            uint32_t hoverAddr = 0xFFFF;
            const char* regionLabel = "Unknown";
            uint16_t regionStart = 0, regionEnd = 0;
            for (size_t i = 0; i < flat.size(); ++i) {
                if (mouse.y >= regionY0[i] && mouse.y < regionY1[i]) {
                    const float t = (mouse.y - regionY0[i]) / (regionY1[i] - regionY0[i]);
                    const uint32_t span = static_cast<uint32_t>(flat[i].end) - flat[i].start + 1;
                    hoverAddr = flat[i].start + static_cast<uint32_t>(t * span);
                    if (hoverAddr > 0xFFFF) hoverAddr = 0xFFFF;
                    regionLabel = flat[i].label;
                    regionStart = flat[i].start;
                    regionEnd   = flat[i].end;
                    break;
                }
            }
            dl->AddLine(ImVec2(barX, mouse.y), ImVec2(barX + barW, mouse.y),
                        IM_COL32(255, 255, 255, 100), 1.0f);

            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "$%04X",
                               static_cast<uint16_t>(hoverAddr));
            ImGui::Text("%s", regionLabel);
            const uint32_t sz = static_cast<uint32_t>(regionEnd) - regionStart + 1;
            if (sz >= 1024)
                ImGui::TextDisabled("$%04X-$%04X (%u KB)", regionStart, regionEnd, (unsigned)(sz / 1024));
            else
                ImGui::TextDisabled("$%04X-$%04X (%u B)",  regionStart, regionEnd, (unsigned)sz);
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                showMemViewer = true;
                memViewer.navigateToAddress(static_cast<int>(hoverAddr));
            }
        }
    }

    const float pcLabelW = ImGui::CalcTextSize("PC $FFFF").x;
    const float rightMargin = 5.0f + 6.0f /*arrow sz*/ + pcLabelW + 6.0f;
    ImGui::Dummy(ImVec2(gutterW + barW + labelGap + maxLabelW + labelGap + bar2W + rightMargin,
                        totalBarH));
    ImGui::End();
}

// ─── Horizontal bar (wide / short) ────────────────────────────────────────

void MainWindow::renderMemoryBarHorizontalWindow()
{
    ImGui::SetNextWindowSize(ImVec2(720, 88), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Map Bar (Horizontal)", &showMemoryBarH)) {
        ImGui::End();
        return;
    }

    auto regions = buildMemoryRegions();

    std::array<uint8_t, 0x10000> ramSnap;
    uint16_t pc;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        std::memcpy(ramSnap.data(), controller.memory().data(), 0x10000);
        pc = controller.cpu().getProgramCounter();
    }

    PageInfo pageMap[256];
    buildPageMap(regions, ramSnap.data(), pageMap);
    const auto flat = flattenPageMap(pageMap);

    // --- Layout ---
    const float textH       = ImGui::GetTextLineHeight();
    const float topAxisH    = textH + 2.0f;     // "$0000"   "$FFFF"
    const float bar1H       = 26.0f;
    const float minRegionW  = 3.0f;
    const ImVec2 origin     = ImGui::GetCursorScreenPos();
    const ImVec2 avail      = ImGui::GetContentRegionAvail();
    const float availW      = std::max(120.0f, avail.x - 4.0f);
    ImDrawList*  dl         = ImGui::GetWindowDrawList();
    const float barY0       = origin.y + topAxisH;
    const float barY1       = barY0 + bar1H;

    // --- Distorted widths (min-width per region, rescale on overflow) ---
    std::vector<float> regionW(flat.size());
    float totalClamped = 0.0f;
    for (size_t i = 0; i < flat.size(); ++i) {
        float natural = (static_cast<float>(flat[i].end - flat[i].start + 1) / 65536.0f) * availW;
        regionW[i] = std::max(natural, minRegionW);
        totalClamped += regionW[i];
    }
    if (totalClamped > availW) {
        float excess = totalClamped - availW;
        float shrinkable = 0.0f;
        for (size_t i = 0; i < flat.size(); ++i)
            if (regionW[i] > minRegionW) shrinkable += (regionW[i] - minRegionW);
        if (shrinkable > 0.0f) {
            float ratio = std::min(1.0f, excess / shrinkable);
            for (size_t i = 0; i < flat.size(); ++i) {
                if (regionW[i] > minRegionW)
                    regionW[i] -= (regionW[i] - minRegionW) * ratio;
            }
        }
    }
    std::vector<float> regionX0(flat.size()), regionX1(flat.size());
    {
        float curX = origin.x;
        for (size_t i = 0; i < flat.size(); ++i) {
            regionX0[i] = curX;
            regionX1[i] = curX + regionW[i];
            curX += regionW[i];
        }
    }
    const float totalBarW = regionX1.empty() ? 0.0f : (regionX1.back() - origin.x);

    // --- Outer frame ---
    dl->AddRect(ImVec2(origin.x - 1, barY0 - 1),
                ImVec2(origin.x + totalBarW + 1, barY1 + 1),
                IM_COL32(80, 80, 80, 255), 2.0f, 0, 1.0f);

    // --- Bar segments ---
    for (size_t ri = 0; ri < flat.size(); ++ri) {
        const auto& fr = flat[ri];
        dl->AddRectFilled(ImVec2(regionX0[ri], barY0),
                          ImVec2(regionX1[ri], barY1), fr.color);
        if (regionW[ri] > 4.0f) {
            dl->AddLine(ImVec2(regionX0[ri] + 0.5f, barY0),
                        ImVec2(regionX0[ri] + 0.5f, barY1),
                        lighten(fr.color, 40), 1.0f);
            dl->AddLine(ImVec2(regionX1[ri] - 0.5f, barY0),
                        ImVec2(regionX1[ri] - 0.5f, barY1),
                        darken(fr.color, 40), 1.0f);
        }
        dl->AddLine(ImVec2(regionX1[ri], barY0),
                    ImVec2(regionX1[ri], barY1),
                    IM_COL32(30, 30, 30, 200), 1.0f);
    }

    auto addrToX = [&](uint32_t addr) -> float {
        for (size_t i = 0; i < flat.size(); ++i) {
            const uint32_t rStart = flat[i].start;
            const uint32_t rEnd   = static_cast<uint32_t>(flat[i].end) + 1;
            if (addr >= rStart && addr < rEnd) {
                const float t = static_cast<float>(addr - rStart) / static_cast<float>(rEnd - rStart);
                return regionX0[i] + t * (regionX1[i] - regionX0[i]);
            }
        }
        return regionX1.back();
    };

    // --- Top axis ---
    {
        const float endX = origin.x + totalBarW;
        const float startTextW = ImGui::CalcTextSize("$0000").x;
        const float endTextW   = ImGui::CalcTextSize("$FFFF").x;
        dl->AddText(ImVec2(origin.x, origin.y),
                    IM_COL32(180, 180, 180, 255), "$0000");
        dl->AddText(ImVec2(endX - endTextW, origin.y),
                    IM_COL32(180, 180, 180, 255), "$FFFF");

        const float minSpacing = 6.0f;
        float lastLabelRight = origin.x + startTextW + minSpacing;
        const float endLabelLeft = endX - endTextW - minSpacing;
        for (int kb = 8; kb < 64; kb += 8) {
            const uint32_t addr = static_cast<uint32_t>(kb) * 1024;
            const float x = addrToX(addr);
            dl->AddLine(ImVec2(x, barY0), ImVec2(x, barY1),
                        IM_COL32(255, 255, 255, 28), 1.0f);
            char tick[8];
            std::snprintf(tick, sizeof(tick), "$%04X", static_cast<uint16_t>(addr));
            const float tw = ImGui::CalcTextSize(tick).x;
            const float lx = x - tw * 0.5f;
            if (lx >= lastLabelRight && (lx + tw) <= endLabelLeft) {
                dl->AddText(ImVec2(lx, origin.y),
                            IM_COL32(110, 110, 110, 255), tick);
                lastLabelRight = lx + tw + minSpacing;
            }
        }
    }

    // --- Inline region labels (only when the bar segment is wide enough) ---
    auto luminance = [](ImU32 c) -> int {
        const int r = (c >>  0) & 0xFF;
        const int g = (c >>  8) & 0xFF;
        const int b = (c >> 16) & 0xFF;
        return (r * 299 + g * 587 + b * 114) / 1000;
    };
    for (size_t ri = 0; ri < flat.size(); ++ri) {
        const auto& fr = flat[ri];
        const float w = regionW[ri];
        if (w < 28.0f) continue;
        const char* lbl = fr.label;
        const float tw = ImGui::CalcTextSize(lbl).x;
        if (tw + 6.0f > w) continue;
        const float cx = (regionX0[ri] + regionX1[ri]) * 0.5f - tw * 0.5f;
        const float cy = (barY0 + barY1) * 0.5f - textH * 0.5f;
        const ImU32 fg = (luminance(fr.color) >= 128)
                          ? IM_COL32(20, 20, 20, 255)
                          : IM_COL32(235, 235, 235, 255);
        dl->AddText(ImVec2(cx, cy), fg, lbl);
    }

    // --- PC triangle below + label ---
    {
        const float pcX = addrToX(pc);
        const float sz  = 5.0f;
        const float ay  = barY1 + 1.0f;
        dl->AddTriangleFilled(
            ImVec2(pcX - sz, ay + sz),
            ImVec2(pcX + sz, ay + sz),
            ImVec2(pcX,      ay),
            IM_COL32(255, 255, 255, 255));
        char pcLabel[16];
        std::snprintf(pcLabel, sizeof(pcLabel), "PC $%04X", pc);
        const float lw = ImGui::CalcTextSize(pcLabel).x;
        float lx = pcX - lw * 0.5f;
        if (lx < origin.x) lx = origin.x;
        if (lx + lw > origin.x + totalBarW) lx = origin.x + totalBarW - lw;
        const float ly = barY1 + sz + 4.0f;
        dl->AddRectFilled(
            ImVec2(lx - 2, ly - 1),
            ImVec2(lx + lw + 2, ly + ImGui::GetTextLineHeight()),
            IM_COL32(40, 40, 50, 220), 2.0f);
        dl->AddText(ImVec2(lx, ly),
                    IM_COL32(255, 255, 255, 255), pcLabel);
    }

    // --- Memory viewer viewport overlay ---
    if (showMemViewer) {
        const auto vp = memViewer.getViewportRange();
        float vpX0 = addrToX(vp.startAddress);
        float vpX1 = addrToX(vp.endAddress);
        if (vpX1 - vpX0 < 4.0f) vpX1 = vpX0 + 4.0f;
        dl->AddRectFilled(ImVec2(vpX0, barY0), ImVec2(vpX1, barY1),
                          IM_COL32(255, 255, 255, 35));
        dl->AddRect(ImVec2(vpX0, barY0), ImVec2(vpX1, barY1),
                    IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
        const float wingH = 4.0f;
        dl->AddLine(ImVec2(vpX0, barY0 - wingH), ImVec2(vpX0, barY0),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(vpX1, barY0 - wingH), ImVec2(vpX1, barY0),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(vpX0, barY1), ImVec2(vpX0, barY1 + wingH),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        dl->AddLine(ImVec2(vpX1, barY1), ImVec2(vpX1, barY1 + wingH),
                    IM_COL32(255, 255, 255, 200), 1.5f);
    }

    // --- Tooltip + click ---
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y >= barY0 && mouse.y < barY1 &&
            mouse.x >= origin.x && mouse.x < origin.x + totalBarW) {
            uint32_t hoverAddr = 0xFFFF;
            const char* regionLabel = "Unknown";
            uint16_t regionStart = 0, regionEnd = 0;
            for (size_t i = 0; i < flat.size(); ++i) {
                if (mouse.x >= regionX0[i] && mouse.x < regionX1[i]) {
                    const float t = (mouse.x - regionX0[i]) / (regionX1[i] - regionX0[i]);
                    const uint32_t span = static_cast<uint32_t>(flat[i].end) - flat[i].start + 1;
                    hoverAddr = flat[i].start + static_cast<uint32_t>(t * span);
                    if (hoverAddr > 0xFFFF) hoverAddr = 0xFFFF;
                    regionLabel = flat[i].label;
                    regionStart = flat[i].start;
                    regionEnd   = flat[i].end;
                    break;
                }
            }
            dl->AddLine(ImVec2(mouse.x, barY0), ImVec2(mouse.x, barY1),
                        IM_COL32(255, 255, 255, 100), 1.0f);

            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "$%04X",
                               static_cast<uint16_t>(hoverAddr));
            ImGui::Text("%s", regionLabel);
            const uint32_t sz = static_cast<uint32_t>(regionEnd) - regionStart + 1;
            if (sz >= 1024)
                ImGui::TextDisabled("$%04X-$%04X (%u KB)", regionStart, regionEnd, (unsigned)(sz / 1024));
            else
                ImGui::TextDisabled("$%04X-$%04X (%u B)",  regionStart, regionEnd, (unsigned)sz);
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                showMemViewer = true;
                memViewer.navigateToAddress(static_cast<int>(hoverAddr));
            }
        }
    }

    const float totalH = topAxisH + bar1H + 6.0f + textH + 4.0f;
    ImGui::Dummy(ImVec2(totalBarW, totalH));
    ImGui::End();
}

// ─── 16 × 16 page grid ────────────────────────────────────────────────────

void MainWindow::renderMemoryGridWindow()
{
    ImGui::SetNextWindowSize(ImVec2(880, 580), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Map Grid", &showMemoryGrid)) {
        ImGui::End();
        return;
    }

    auto regions = buildMemoryRegions();
    const int numRegions = static_cast<int>(regions.size());

    std::array<uint8_t, 0x10000> ramSnap;
    uint16_t pc;
    uint8_t  sp;
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        std::memcpy(ramSnap.data(), controller.memory().data(), 0x10000);
        pc = controller.cpu().getProgramCounter();
        sp = controller.cpu().getStackPointer();
    }
    const int pcPage = pc >> 8;
    const int spPage = 1;  // stack always lives in page 1

    const int   COLS = 16;
    const int   ROWS = 16;
    const float cellSize = 16.0f;
    const float spacing  = 1.0f;
    const float gridW = COLS * (cellSize + spacing);
    const float gridH = ROWS * (cellSize + spacing);
    const float mapColW = gridW + 40.0f;

    ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH
                               | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("MemoryMapGrid", 2, tableFlags)) {
        ImGui::TableSetupColumn("left",  ImGuiTableColumnFlags_WidthFixed, mapColW);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch);

        // ── Row 0: grid + legend ────────────────────────────────────────
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Map (1 cell = 256 bytes):");
        ImGui::Spacing();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (int row = 0; row < ROWS; ++row) {
            for (int col = 0; col < COLS; ++col) {
                const int page = row * COLS + col;
                const uint16_t addr = static_cast<uint16_t>(page << 8);

                ImU32 baseColor = IM_COL32(40, 40, 40, 255);
                for (int r = 0; r < numRegions; ++r) {
                    if (addr >= regions[r].start && addr <= regions[r].end) {
                        baseColor = regions[r].color;
                    }
                }

                bool hasData = false;
                const bool isUserRam = (baseColor == kRamColor);
                if (isUserRam) {
                    for (int b = 0; b < 256; ++b) {
                        if (ramSnap[addr + b] != 0) { hasData = true; break; }
                    }
                }

                ImU32 cellColor = baseColor;
                if (isUserRam && !hasData) cellColor = IM_COL32( 20,  60,  20, 255);
                else if (isUserRam)        cellColor = IM_COL32( 80, 220,  80, 255);

                const float x = origin.x + col * (cellSize + spacing);
                const float y = origin.y + row * (cellSize + spacing);
                const ImVec2 p0(x, y);
                const ImVec2 p1(x + cellSize, y + cellSize);

                drawList->AddRectFilled(p0, p1, cellColor);

                if (page == pcPage)
                    drawList->AddRect(p0, p1, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
                if (page == spPage) {
                    const ImVec2 inner0(p0.x + 1, p0.y + 1);
                    const ImVec2 inner1(p1.x - 1, p1.y - 1);
                    drawList->AddRect(inner0, inner1, IM_COL32(255, 165, 0, 255), 0.0f, 0, 1.0f);
                }

                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                    const ImVec2 mousePos = ImGui::GetMousePos();
                    if (mousePos.x >= p0.x && mousePos.x < p1.x &&
                        mousePos.y >= p0.y && mousePos.y < p1.y) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            showMemViewer = true;
                            memViewer.navigateToAddress(addr);
                        }
                        ImGui::BeginTooltip();
                        ImGui::Text("Page $%02X : $%04X-$%04X", page, addr, addr + 0xFF);
                        const char* tooltipLabel = nullptr;
                        for (int r = 0; r < numRegions; ++r) {
                            if (addr >= regions[r].start && addr <= regions[r].end)
                                tooltipLabel = regions[r].label;
                        }
                        if (tooltipLabel) ImGui::Text("%s", tooltipLabel);
                        if (page == pcPage) ImGui::Text("PC = $%04X", pc);
                        if (page == spPage) ImGui::Text("SP = $01%02X", sp);
                        ImGui::EndTooltip();
                    }
                }
            }
        }

        // 4 KB row labels on the right.
        const float rightMargin = origin.x + gridW + 4.0f;
        for (int row = 0; row < ROWS; ++row) {
            const float y = origin.y + row * (cellSize + spacing) + 2;
            const int kb = (row + 1) * 4;
            char label[16];
            std::snprintf(label, sizeof(label), "%dK", kb);
            drawList->AddText(ImVec2(rightMargin, y),
                              IM_COL32(150, 150, 150, 255), label);
        }

        ImGui::Dummy(ImVec2(mapColW, gridH));
        ImGui::Text("PC = $%04X  SP = $01%02X", pc, sp);

        // ── Legend column ───────────────────────────────────────────────
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Legend:");
        ImGui::Separator();

        // Skip the layer-0 User-RAM blanket so the legend is one entry per
        // labelled zone, deduped on (color, label).
        std::vector<std::pair<ImU32, const char*>> seen;
        for (int i = 1; i < numRegions; ++i) {
            bool dup = false;
            for (const auto& s : seen) {
                if (s.first == regions[i].color
                    && std::strcmp(s.second, regions[i].label) == 0) { dup = true; break; }
            }
            if (dup) continue;
            seen.push_back({ regions[i].color, regions[i].label });
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + 12, p.y + 12), regions[i].color);
            dl->AddRect(p, ImVec2(p.x + 12, p.y + 12), IM_COL32(180, 180, 180, 255));
            ImGui::Dummy(ImVec2(16, 14));
            ImGui::SameLine();
            ImGui::Text("$%04X-$%04X %s",
                        regions[i].start, regions[i].end, regions[i].label);
        }
        // User RAM gets one line at the end (we don't duplicate it per
        // labelled span — it's the blanket).
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + 12, p.y + 12), kRamColor);
            dl->AddRect(p, ImVec2(p.x + 12, p.y + 12), IM_COL32(180, 180, 180, 255));
            ImGui::Dummy(ImVec2(16, 14));
            ImGui::SameLine();
            ImGui::Text("User RAM (heat-mapped)");
        }

        // ── Row 1: Apple II I/O reference + CPU vectors ─────────────────
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Soft switches & I/O:");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "  Keyboard");
        ImGui::BulletText("$C000  KBD     - data + strobe (bit 7)");
        ImGui::BulletText("$C010  KBDSTRB - clear strobe (R/W)");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "  Speaker / paddles");
        ImGui::BulletText("$C030-$C03F  Speaker toggle (any access)");
        ImGui::BulletText("$C061-$C063  PB0..PB2 (sign bit = pressed)");
        ImGui::BulletText("$C064-$C067  PADDL0..3 (RC discharge timer)");
        ImGui::BulletText("$C070        Paddle reset latch");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "  Display soft switches");
        ImGui::BulletText("$C050/$C051  Graphics / Text");
        ImGui::BulletText("$C052/$C053  Full-screen / Mixed");
        ImGui::BulletText("$C054/$C055  Page 1 / Page 2");
        ImGui::BulletText("$C056/$C057  Lo-res / Hi-res");
        if (diskCard != nullptr) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "  Disk II (slot 6)");
            ImGui::BulletText("$C0E0-$C0E7  Phases 0-3 OFF/ON");
            ImGui::BulletText("$C0E8/$C0E9  Motor OFF / ON");
            ImGui::BulletText("$C0EA/$C0EB  Drive 1 / Drive 2");
            ImGui::BulletText("$C0EC/$C0ED  Q6L / Q6H (shift / load)");
            ImGui::BulletText("$C0EE/$C0EF  Q7L / Q7H (read / write)");
            ImGui::BulletText("$C600-$C6FF  P5A boot PROM");
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("CPU vectors:");
        ImGui::BulletText("$FFFA/B  NMI   -> $%04X",
            (int)ramSnap[0xFFFA] | ((int)ramSnap[0xFFFB] << 8));
        ImGui::BulletText("$FFFC/D  RESET -> $%04X",
            (int)ramSnap[0xFFFC] | ((int)ramSnap[0xFFFD] << 8));
        ImGui::BulletText("$FFFE/F  IRQ   -> $%04X",
            (int)ramSnap[0xFFFE] | ((int)ramSnap[0xFFFF] << 8));

        ImGui::EndTable();
    }

    ImGui::End();
}
