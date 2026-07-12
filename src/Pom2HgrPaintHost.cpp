// POM2 Apple II Emulator
// Copyright (C) 2026 Verhille Arnaud
//
// Pom2HgrPaintHost — see Pom2HgrPaintHost.h.

#include "Pom2HgrPaintHost.h"

#include "Apple2Display.h"
#include "EmulationController.h"
#include "HgrPaintModel.h"        // hgrpaint:: geometry constants
#include "Memory.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// The portable hgrpaint/ module only *declares* the stb entry points
// (HgrImageDecode.cpp calls stbi_load, savePng below calls stbi_write_png);
// the host app owns the single non-static implementation. MainWindow.cpp's
// copy is STB_IMAGE_STATIC (TU-internal, About-photo only), so this is the
// one that actually links against hgrpaint/.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace {

// Opaque handle behind IHgrPaintHost's void* texture API.
struct GlTex {
    GLuint id = 0;
    int w = 0, h = 0;
    bool linear = false;
};

} // namespace

Pom2HgrPaintHost::Pom2HgrPaintHost(EmulationController* emu)
    : emu_(emu),
      writer_([this](const PaintCardBatcher::Writes& w) {
          if (!emu_) return;
          std::lock_guard<std::mutex> lk(emu_->stateMutex());
          Memory& mem = emu_->memory();
          for (const auto& [addr, val] : w)
              if (addr < 0xC000) mem.writeRamUnchecked(addr, val);
      }),
      auxWriter_([this](const PaintCardBatcher::Writes& w) {
          if (!emu_) return;
          std::lock_guard<std::mutex> lk(emu_->stateMutex());
          uint8_t* aux = emu_->memory().auxDataMutable();
          for (const auto& [addr, val] : w)
              if (addr < 0xC000) aux[addr] = val;
      })
{
}

Pom2HgrPaintHost::~Pom2HgrPaintHost() = default;

// Pokes bypass the IIe paging switches on purpose (writeRamUnchecked / the
// raw aux bank): the editor edits a specific plane of a specific page, and
// must keep doing so even while the live machine has 80STORE/RAMWRT active.
void Pom2HgrPaintHost::pokeByte(uint16_t addr, uint8_t value) { writer_.poke(addr, value); }
void Pom2HgrPaintHost::pokeAuxByte(uint16_t addr, uint8_t value) { auxWriter_.poke(addr, value); }
void Pom2HgrPaintHost::beginBatch() { writer_.begin(); auxWriter_.begin(); }
void Pom2HgrPaintHost::endBatch()   { auxWriter_.end(); writer_.end(); }

// Flip the live machine's video soft switches so the on-screen picture
// follows the page the editor is editing — the same $C05x writes a program
// would perform. IIe-only latches (80COL, AN3/DHIRES) are switched off so a
// previous DHGR selection doesn't linger; Memory ignores them on a II+.
void Pom2HgrPaintHost::setDisplayMode(bool grMode, bool page2)
{
    if (!emu_) return;
    std::lock_guard<std::mutex> lk(emu_->stateMutex());
    Memory& mem = emu_->memory();
    mem.memWrite(0xC050, 0);                       // GRAPHICS
    mem.memWrite(0xC052, 0);                       // full screen (MIXED off)
    mem.memWrite(page2 ? 0xC055 : 0xC054, 0);      // page select
    mem.memWrite(grMode ? 0xC056 : 0xC057, 0);     // LORES / HIRES
    if (mem.isIIE()) {
        mem.memWrite(0xC00C, 0);                   // 80COL off
        mem.memWrite(0xC05F, 0);                   // AN3 / DHIRES off
    }
}

void Pom2HgrPaintHost::setDisplayModeDhgr(bool page2)
{
    if (!emu_) return;
    std::lock_guard<std::mutex> lk(emu_->stateMutex());
    Memory& mem = emu_->memory();
    if (!mem.isIIE()) return;
    mem.memWrite(0xC050, 0);                       // GRAPHICS
    mem.memWrite(0xC052, 0);                       // full screen
    mem.memWrite(page2 ? 0xC055 : 0xC054, 0);      // page select
    mem.memWrite(0xC057, 0);                       // HIRES
    mem.memWrite(0xC00D, 0);                       // 80COL on
    mem.memWrite(0xC05E, 0);                       // AN3 / DHIRES on
}

bool Pom2HgrPaintHost::supportsDhgr() const
{
    return emu_ && emu_->memory().isIIE();
}

// ── Offscreen canvas render ──────────────────────────────────────────────────
// A private, never-clocked Memory (IIe mode, so one rig serves HGR, lo-res GR
// and DHGR) + Apple2Display pair. Its cycle counter never advances, so its
// video-event log never publishes a frame: render() always takes the fast
// single-state renderInternal path over exactly the bytes staged below.

void Pom2HgrPaintHost::ensureScratch()
{
    if (scratch_) return;
    scratch_ = std::make_unique<Memory>();
    scratch_->setIIEMode(true);
    gfx_ = std::make_unique<Apple2Display>();
    gfx_->setAuxMemory(scratch_->auxData());
}

void Pom2HgrPaintHost::renderScratch(ScratchMode m, const uint8_t* main8k,
                                     const uint8_t* aux8k, uint32_t* outRgba,
                                     bool mono)
{
    ensureScratch();

    if (!scratchStaged_ || scratchMode_ != m) {
        scratch_->memWrite(0xC050, 0);             // GRAPHICS
        scratch_->memWrite(0xC052, 0);             // full screen
        scratch_->memWrite(0xC054, 0);             // page 1 (pages share layout)
        scratch_->memWrite(m == ScratchMode::Gr ? 0xC056 : 0xC057, 0);
        scratch_->memWrite(m == ScratchMode::Dhgr ? 0xC00D : 0xC00C, 0);
        scratch_->memWrite(m == ScratchMode::Dhgr ? 0xC05E : 0xC05F, 0);
        scratchMode_   = m;
        scratchStaged_ = true;
    }

    switch (m) {
    case ScratchMode::Hgr:
        for (int i = 0; i < hgrpaint::kHiresSize; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), main8k[i]);
        break;
    case ScratchMode::Gr:
        // Lo-res: the first 1 KB of the editor page is the text/lo-res page.
        for (int i = 0; i < 0x400; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), main8k[i]);
        break;
    case ScratchMode::Dhgr:
        for (int i = 0; i < hgrpaint::kHiresSize; ++i)
            scratch_->writeRamUnchecked(static_cast<uint16_t>(0x2000 + i), main8k[i]);
        std::memcpy(scratch_->auxDataMutable() + 0x2000, aux8k, hgrpaint::kHiresSize);
        break;
    }

    // Canvas look: MAME-LUT NTSC colour / white-phosphor mono (decay 0 — no
    // afterglow ghosting on erase). Deliberately independent of the user's
    // on-screen HiResMode so the canvas stays deterministic.
    gfx_->setHiResMode(mono ? Apple2Display::HiResMode::MonoWhite
                            : Apple2Display::HiResMode::ColorNTSC);
    gfx_->render(*scratch_);
    std::copy(gfx_->pixels(),
              gfx_->pixels() + static_cast<size_t>(gfx_->width()) * gfx_->height(),
              outRgba);
}

void Pom2HgrPaintHost::renderHgrPage(const uint8_t* page8k, uint32_t* outRgba,
                                     bool mono, bool grMode)
{
    renderScratch(grMode ? ScratchMode::Gr : ScratchMode::Hgr,
                  page8k, nullptr, outRgba, mono);
}

void Pom2HgrPaintHost::renderDhgrPage(const uint8_t* aux8k, const uint8_t* main8k,
                                      uint32_t* outRgba, bool mono)
{
    renderScratch(ScratchMode::Dhgr, main8k, aux8k, outRgba, mono);
}

// ── File I/O ─────────────────────────────────────────────────────────────────

bool Pom2HgrPaintHost::loadImage(const std::string& path, uint16_t baseAddr,
                                 std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) { err = "empty file"; return false; }
    const size_t n = std::min(bytes.size(), static_cast<size_t>(0xC000 - baseAddr));
    std::lock_guard<std::mutex> lk(emu_->stateMutex());
    Memory& mem = emu_->memory();
    for (size_t i = 0; i < n; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(baseAddr + i),
                              static_cast<uint8_t>(bytes[i]));
    return true;
}

bool Pom2HgrPaintHost::saveImage(const std::string& path, uint16_t baseAddr,
                                 int sizeBytes, std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    if (sizeBytes <= 0) sizeBytes = hgrpaint::kHiresSize;
    sizeBytes = std::min<int>(sizeBytes, 0xC000 - baseAddr);
    std::vector<uint8_t> bytes(static_cast<size_t>(sizeBytes));
    {
        std::lock_guard<std::mutex> lk(emu_->stateMutex());
        std::memcpy(bytes.data(), emu_->memory().data() + baseAddr, bytes.size());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot create " + path; return false; }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) { err = "write failed (disk full?)"; return false; }
    return true;
}

bool Pom2HgrPaintHost::loadDhgrImage(const std::string& path, uint16_t baseAddr,
                                     std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 2 * static_cast<size_t>(hgrpaint::kHiresSize)) {
        err = "not a 16 KB DHGR (A2FC) dump: " + path;
        return false;
    }
    std::lock_guard<std::mutex> lk(emu_->stateMutex());
    Memory& mem = emu_->memory();
    std::memcpy(mem.auxDataMutable() + baseAddr, bytes.data(), hgrpaint::kHiresSize);
    for (int i = 0; i < hgrpaint::kHiresSize; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(baseAddr + i),
                              static_cast<uint8_t>(bytes[hgrpaint::kHiresSize + i]));
    return true;
}

bool Pom2HgrPaintHost::saveDhgrImage(const std::string& path, uint16_t baseAddr,
                                     std::string& err)
{
    if (!emu_) { err = "no emulator"; return false; }
    std::vector<uint8_t> bytes(2 * static_cast<size_t>(hgrpaint::kHiresSize));
    {
        std::lock_guard<std::mutex> lk(emu_->stateMutex());
        const Memory& mem = emu_->memory();
        std::memcpy(bytes.data(), mem.auxData() + baseAddr, hgrpaint::kHiresSize);
        std::memcpy(bytes.data() + hgrpaint::kHiresSize, mem.data() + baseAddr,
                    hgrpaint::kHiresSize);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot create " + path; return false; }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) { err = "write failed (disk full?)"; return false; }
    return true;
}

bool Pom2HgrPaintHost::savePng(const std::string& path, const uint32_t* rgba,
                               int w, int h, std::string& err)
{
    // rgba is top-down RGBA8, exactly what stbi_write_png expects with
    // stride = w*4.
    if (stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) == 0) {
        err = "stbi_write_png failed (directory writable?)";
        return false;
    }
    return true;
}

// ── GL texture plumbing ──────────────────────────────────────────────────────
// Same-size repeat uploads (the steady state at ~60 Hz while painting)
// sub-update the existing texture; only a dimension/filter change
// destroys-and-recreates.

void* Pom2HgrPaintHost::uploadTexture(void* tex, const void* rgba,
                                      int w, int h, bool linear)
{
    auto* t = static_cast<GlTex*>(tex);
    if (t && t->w == w && t->h == h && t->linear == linear) {
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return t;
    }
    if (t) destroyTexture(t);
    t = new GlTex{0, w, h, linear};
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    const GLint filt = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

void Pom2HgrPaintHost::destroyTexture(void* tex)
{
    auto* t = static_cast<GlTex*>(tex);
    if (!t) return;
    if (t->id) glDeleteTextures(1, &t->id);
    delete t;
}

ImTextureID Pom2HgrPaintHost::textureToImTexture(void* tex) const
{
    auto* t = static_cast<GlTex*>(tex);
    return t ? (ImTextureID)(uintptr_t)t->id : (ImTextureID)0;
}
