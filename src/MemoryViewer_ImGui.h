// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Memory viewer / hex editor. Hex grid + ASCII column, region-coloured by
// Apple II zone (zero page, stack, text pages, HGR pages, I/O, ROM).
// Inline edit with undo/redo, search, change highlighting, and a togglable
// 6502 disassembly view sharing the same address cursor.

#ifndef POM2_MEMORY_VIEWER_IMGUI_H
#define POM2_MEMORY_VIEWER_IMGUI_H

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Memory;

class MemoryViewer_ImGui
{
public:
    explicit MemoryViewer_ImGui(Memory* memory);

    // Render the entire viewer window contents. Caller owns Begin()/End()
    // and decides where the window sits in the layout.
    void render();

    // Programmatic navigation — used by future "go to PC" buttons or a
    // disassembly listing that wants to centre on a label.
    void navigateToAddress(int address);

    // Inclusive byte range currently visible in the hex grid. Used by the
    // memory-map widgets to draw a viewport overlay so the bar and the
    // viewer stay in sync visually.
    struct ViewportRange { int startAddress; int endAddress; };
    ViewportRange getViewportRange() const {
        const int span = bytesPerRow * displayRows;
        const int end  = std::min(0xFFFF, startAddress + span - 1);
        return { startAddress, end };
    }

    // Hook fired when the user edits a byte. MainWindow plumbs this through
    // EmulationController so the write goes through Memory::memWrite under
    // stateMutex (rather than scribbling on the raw array).
    void setWriteCallback(std::function<void(uint16_t, uint8_t)> cb) {
        writeCallback = std::move(cb);
    }

    // Push the live CPU mode so the Disasm view decodes 65C02 opcodes
    // correctly (and doesn't desync on 3-byte BBR/BBS). Set each frame.
    void setCmosMode(bool on) { cmosDisasm_ = on; }

private:
    Memory* memory;
    std::function<void(uint16_t, uint8_t)> writeCallback;
    bool cmosDisasm_ = false;

    // Layout state.
    int  startAddress  = 0x0000;
    int  bytesPerRow   = 16;
    int  displayRows   = 32;
    bool showAscii     = true;
    bool showDisasm    = false;
    bool showChanges   = true;
    bool colorizeRegions = true;

    // Change-highlight tracking. Per-byte frame counter — bytes touched in
    // the last `kChangeFadeFrames` ticks render with an orange flash.
    std::vector<uint8_t>  prevMemory;
    std::vector<uint32_t> changeFrame;
    uint32_t              frameCounter = 0;
    static constexpr uint32_t kChangeFadeFrames = 45;  // ~0.75 s at 60 fps

    // Search.
    char searchBuffer[256] = {0};
    int  searchAddress     = -1;
    bool showSearch        = false;
    bool searchAscii       = false;

    // Inline edit. Double-click on a byte arms editAddress for one frame.
    int  editAddress  = -1;
    char editBuffer[4] = {0};
    bool editFocusSet  = false;

    struct EditRecord { uint16_t address; uint8_t oldValue; uint8_t newValue; };
    std::vector<EditRecord> undoStack;
    std::vector<EditRecord> redoStack;

    // Bookmarks.
    std::vector<int> bookmarks;

    // Helpers.
    void renderControls();
    void renderRegionBanner();
    void renderHexView();
    void renderDisasmView();
    void renderSearchDialog();
    void handleNavigation();
    void detectChanges();

    void jumpToAddress(int address);
    void searchHexBytes();
    void searchAsciiString();
    void applyEdit(uint16_t address, uint8_t newValue);
    void undo();
    void redo();

    uint8_t        readByte(int address) const;
    const uint8_t* memoryPointer() const;
    static char    printable(uint8_t v);

    ImVec4      regionColour(int address) const;
    const char* regionName  (int address) const;
};

#endif // POM2_MEMORY_VIEWER_IMGUI_H
