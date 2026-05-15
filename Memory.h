// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Apple II / II+ / IIe memory.
//
// II+: 48 KB RAM ($0000-$BFFF), I/O page ($C000-$C0FF), slot ROM area
// ($C100-$C7FF), 12 KB Monitor + Applesoft ROM ($D000-$FFFF), 16 KB
// Language Card overlay. Soft switches at $C050-$C057 drive display modes.
//
// IIe (when isIIE() is true): adds a 64 KB auxiliary bank, a 4 KB internal
// I/O ROM at $C100-$CFFF (motherboard firmware including the slot-3
// 80-column driver), an aux Language Card overlay, and the IIe paging
// soft switches at $C000-$C00F (80STORE, RAMRD, RAMWRT, INTCXROM, ALTZP,
// SLOTC3ROM, 80COL, ALTCHAR) with status reads at $C013-$C018, $C01E,
// $C01F. RAM routing per address range:
//   $0000-$01FF      ALTZP        → aux else main
//   $0200-$03FF      RAMRD/RAMWRT → aux else main
//   $0400-$07FF      80STORE on   → PAGE2 picks aux/main; else RAMRD/WRT
//   $0800-$1FFF      RAMRD/RAMWRT → aux else main
//   $2000-$3FFF      80STORE+HIRES on → PAGE2 picks aux/main; else RAMRD/WRT
//   $4000-$BFFF      RAMRD/RAMWRT → aux else main
//   $C100-$CFFF      INTCXROM     → internal IO ROM else slot bus
//                    SLOTC3ROM off → $C300-$C3FF reads internal ROM even
//                                    when INTCXROM is off
//   $D000-$FFFF      ALTZP picks the aux Language Card bank trio

#ifndef POM2_MEMORY_H
#define POM2_MEMORY_H

#include "SlotBus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class CassetteDevice;
class M6502;
class SpeakerDevice;

namespace pom2 { class IWMDevice; }

class Memory
{
public:
    Memory();

    // Built-in cassette interface — Apple II routes $C020 (output toggle)
    // and $C060 (input bit-7) directly to the cassette without a card.
    // Pointer is non-owning; lifetime managed by EmulationController.
    void setCassetteDevice(CassetteDevice* dev) { cassette = dev; }

    /// Built-in speaker (1-bit flip-flop at $C030-$C03F). Non-owning
    /// pointer set by EmulationController. The CPU pointer is needed
    /// by the $C030 handler to timestamp toggles with sub-instruction
    /// precision (`cycleCounter + cpu->getCurrentInstructionCycles()`).
    void setSpeakerDevice(SpeakerDevice* s) { speaker = s; }
    /// Wire the host CPU. Also installs a SlotBus IRQ router so cards
    /// can raise IRQ via `SlotPeripheral::assertIrq()` without each
    /// holding their own M6502*. Pass nullptr to disconnect (the router
    /// is replaced with an empty function so stray assertIrq() calls
    /// from teardown don't dereference a dangling pointer).
    void setCpu(M6502* c);

    /// Apple //c / //c+ on-board IWM controller. Non-owning pointer
    /// set by EmulationController. When iicHasAltBank is on, $C0E0-
    /// $C0EF accesses always mirror to this device so its state
    /// machine (MAME `iwm.cpp` port — see `IWMDevice.{h,cpp}`)
    /// evolves in lock-step with the slot-6 DiskIICard's lightweight
    /// IWM-mode shadow.
    void setIWM(pom2::IWMDevice* iwm) { iwmDevice = iwm; }

    /// When true (default), `$C0E0-$C0EF` reads on iicHasAltBank
    /// profiles return the IWMDevice's value rather than the slot-6
    /// DiskIICard's. Writes are dispatched to both either way.
    /// Setting false reverts to "shadow mode" — IWMDevice still
    /// advances on every access (timer drain, mode/status registers
    /// stay coherent with what the //c+ alt firmware expects), but
    /// the byte the CPU sees comes from DiskIICard's LSS path. Used
    /// during the SmartPort port to A/B-compare the two paths; the
    /// env var `POM2_IWM_LEGACY_DATA_PATH` lets the user flip this
    /// without rebuilding.
    void setIWMAuthoritative(bool on) { iwmAuthoritative = on; }
    bool isIWMAuthoritative() const   { return iwmAuthoritative; }

    /// Apple II expansion bus — slots 0-7. Cards plug directly via the
    /// SlotBus. Memory routes $C080-$CFFF accesses through it.
    SlotBus&       slotBus()       { return slots; }
    const SlotBus& slotBus() const { return slots; }

    /// Flat 64 KB RAM mode — bypasses soft switches, slot bus and ROM
    /// write protection. Every access is plain mem[addr]. Used **only**
    /// by the Klaus Dormann functional test, which expects the whole
    /// address space to behave as RAM. Must NOT be enabled in normal
    /// emulation; no safety checks remain.
    void setTestMode(bool enabled) { testMode = enabled; }
    bool isTestMode() const        { return testMode; }

    // CPU bus interface (called from M6502).
    uint8_t memRead(uint16_t addr);
    void    memWrite(uint16_t addr, uint8_t value);

    // Diagnostic — used by M6502's BRK trace.
    std::string busStateSummary() const;

    // ROM loading. Apple II distributions ship as a single 12 KB image
    // covering $D000-$FFFF. Returns 1 on success, 0 on failure (last
    // error in `lastError`).
    //
    // 32 KB dump disambiguation: //e "system + video combined" dumps
    // carry the firmware in the UPPER 16 KB (file offsets 0x4000-0x7FFF)
    // with charset data in the lower half. //c / //c+ dumps instead
    // carry TWO 16 KB firmware banks side-by-side — bank 0 in the LOWER
    // half (the cold-reset entry), bank 1 in the upper half (alt
    // firmware reached via $C028 ROMBANK). The two layouts are
    // indistinguishable from file size alone — the caller passes
    // `pickLower16KFor32K=true` when loading a //c-style dump.
    int loadAppleIIRom(const char* filename, bool pickLower16KFor32K = false);
    int loadCharRom(const char* filename);  // 2 KB, 256 glyphs × 8 rows

    const std::string& getLastError() const { return lastError; }

    // Direct access for the display / debugger / snapshot.
    const uint8_t* data() const { return mem.data(); }
    uint8_t* dataMutable()      { return mem.data(); }

    /// Bulk ROM load — bypasses the writable[] bitmap. Used by RomLoader
    /// (and by future cards) to flash their ROM image into protected
    /// regions without having to flip ROM-protect manually. `addr +
    /// length` must be ≤ $10000.
    bool loadRomBytes(const uint8_t* src, size_t length, uint16_t addr);

    /// Mark `[lo, hi]` as ROM (writes silently dropped). Public so cards
    /// can declare their ROM windows after loading. Idempotent.
    void markRomRange(uint16_t lo, uint16_t hi) { markRomRegion(lo, hi); }

    // Character ROM access (2 KB, 8 bytes/glyph). May be empty if the
    // user hasn't loaded a charset — we then fall back to a built-in
    // ASCII table at render time.
    const std::vector<uint8_t>& charRom() const { return characterRom; }

    // Soft-switch state (read by the display / speaker / paddle code).
    struct DisplayState {
        bool textMode  = true;   // $C050 clear, $C051 set
        bool mixedMode = false;  // $C052 clear, $C053 set
        bool page2     = false;  // $C054 page 1, $C055 page 2
        bool hiRes     = false;  // $C056 lo-res, $C057 hi-res
        // 80COL ($C00C off / $C00D on) and AN3 ($C05E off / $C05F on)
        // are tracked here so the UI can show them. Le Chat Mauve / Video-7
        // also subscribe to per-access edges via SlotBus::broadcastVideoSwitch
        // — a cleared/set boolean is not enough on its own (the FIFO needs
        // every rising edge), but the snapshot is useful for diagnostics.
        bool eightyCol = false;
        bool an3       = false;
        // IIe-only: ALTCHAR ($C00E off / $C00F on) selects between the
        // standard charset (with flashing inverse) and the alternate set
        // (mousetext + non-flashing inverse). Ignored on II+.
        bool altChar   = false;
        // IIe-only: DHIRESON ($C05E) / DHIRESOFF ($C05F). When 80COL is
        // also on, DHGR mode reads aux + main HGR pages interleaved and
        // doubles the horizontal resolution to 560. Ignored on II+
        // (where the same soft switches are pure AN3 annunciator).
        bool dhgr      = false;
        // IIe-only: 80STORE ($C000 off / $C001 on) makes PAGE2 swap text
        // page 1 (and HGR page 1 when HIRES is on) to aux RAM rather than
        // selecting page 2. The display needs this to know whether to read
        // the text page from aux when 80STORE+PAGE2 are both on.
        bool eightyStore = false;
    };
    DisplayState getDisplayState() const {
        std::lock_guard<std::mutex> lk(stateMutex);
        return display;
    }

    // Keyboard bridge — UI thread enqueues keys, CPU thread reads them
    // via $C000 / clears the strobe via $C010.
    void queueKey(uint8_t ascii);       // 7-bit ASCII (upper or lower case)
    void clearKeyStrobe();              // mirrors $C010 access

    /// Paste a block of text. Line-endings normalised to CR ($0D) — `\r\n`,
    /// `\r`, `\n` all collapse to one CR. Non-printable controls below
    /// `$20` other than CR / HT are dropped silently. Capped at
    /// `kPasteMaxChars` so a runaway clipboard can't overwhelm the queue.
    /// Bytes are appended to an internal FIFO that drains one byte per
    /// `clearKeyStrobe()` — so the Apple II ROM's strobe-and-poll loop
    /// pulls them out at exactly its own pace, no timing tricks needed.
    /// Returns the number of bytes actually queued (after filtering).
    static constexpr size_t kPasteMaxChars = 4096;
    size_t pasteText(const char* data, size_t length);
    size_t pasteText(const std::string& s) { return pasteText(s.data(), s.size()); }

    /// Like pasteText but does NOT filter control bytes — used by tests
    /// that need to drive a launcher's arrow keys (Ctrl-J / Ctrl-K) or
    /// ESC. Bytes are stripped to 7 bits but otherwise pass through
    /// verbatim. Same FIFO + strobe drain as pasteText.
    size_t pasteRawKeys(const char* data, size_t length);

    /// How many bytes are still waiting in the paste queue. UI shows this
    /// in the Emulation panel + Edit menu so the user knows the paste is
    /// in flight.
    size_t pendingPasteSize() const;
    void   cancelPaste();

    // Speaker — the CPU toggles a flip-flop by reading $C030. We expose
    // a counter the audio backend can sample. UI may also subscribe via
    // setSpeakerToggleCallback().
    uint64_t getSpeakerToggleCount() const { return speakerToggles.load(); }
    using SpeakerCallback = void (*)(void* user);
    void setSpeakerCallback(SpeakerCallback cb, void* user) {
        speakerCb = cb; speakerUser = user;
    }

    // Paddle inputs — $C064-$C067 read the 4 paddles, $C061-$C063 read
    // the 3 push-button switches. Game-port reset latch armed by $C070.
    void setPaddle(int idx, uint8_t value);    // 0..3
    void setPaddleButton(int idx, bool down);  // 0..2

    // IIe modifier keys. Real //e wires Open Apple to PB0 ($C061),
    // Solid Apple to PB1 ($C062), and Shift (with the SHK jumper set,
    // which is the factory default on enhanced //e) to PB2 ($C063).
    // MAME `apple2e.cpp:1908-1913` honours this. The host keyboard
    // handler can call these from a GLFW key callback when the IIe
    // ROM is loaded; they're OR'd into the joystick button states at
    // read time.
    void setOpenAppleKey  (bool down) { openAppleKey  = down; }
    void setSolidAppleKey (bool down) { solidAppleKey = down; }
    void setShiftKey      (bool down) { shiftKey      = down; }

    // Reset — clears the keyboard strobe, returns to text/page 1 mode.
    // Does NOT touch RAM (matches Apple II reset behaviour: RESET only
    // jumps through ($FFFC) without zeroing memory).
    void resetSoftSwitches();

    // Power-cycle helper: wipe user RAM ($0000-$BFFF). Leaves the I/O page,
    // slot ROM area, and main ROM ($D000-$FFFF) intact. In IIe mode also
    // wipes the auxiliary 64 KB bank and the aux LC banks (but never the
    // internal I/O ROM, which is ROM).
    void clearRam();

    // Apple IIe extension. Off by default — call setIIEMode(true) BEFORE
    // loadAppleIIRom() to switch the loader/dispatcher to IIe behaviour.
    // When false, every IIe-specific code path is gated off and the class
    // behaves as a plain II+.
    void setIIEMode(bool on);
    bool isIIE() const                       { return iieMode; }
    uint16_t iieModeFlags() const            { return iieMemMode; }
    const uint8_t* auxData() const           { return aux.data(); }
    uint8_t*       auxDataMutable()          { return aux.data(); }
    const uint8_t* internalIORomData() const { return internalIORom.data(); }

    /// Applied Engineering RamWorks III — IIe aux-slot RAM expansion up
    /// to 8 MB (128 × 64 KB banks). Verbatim port of MAME
    /// src/devices/bus/a2bus/a2eramworks3.cpp. `banks == 1` = stock IIe
    /// 64 KB aux (no RamWorks). Standard tiers: 1 (stock), 4 (256K),
    /// 8 (512K), 16 (1M), 48 (3M), 128 (8M). Clamped to [1, 128].
    /// Only meaningful in IIe mode — call after setIIEMode(true) and
    /// BEFORE loadAppleIIRom. Wipes aux contents and resets current
    /// bank to 0 (MAME `device_reset` line 67).
    void setRamWorksBanks(uint32_t banks);
    uint32_t ramWorksBanks() const { return ramWorksBanks_; }
    uint8_t  ramWorksBank()  const { return ramWorksBank_; }

    // IIe memory mode flags. Bit positions are arbitrary (we don't need to
    // match AppleWin's MF_* layout); they only have to be stable for the
    // life of the process. Tests pin the routing behaviour, not the flag
    // values themselves.
    static constexpr uint16_t MF_80STORE   = 0x0001;  // $C000/01
    static constexpr uint16_t MF_RAMRD     = 0x0002;  // $C002/03
    static constexpr uint16_t MF_RAMWRT    = 0x0004;  // $C004/05
    static constexpr uint16_t MF_INTCXROM  = 0x0008;  // $C006/07
    static constexpr uint16_t MF_ALTZP     = 0x0010;  // $C008/09
    static constexpr uint16_t MF_SLOTC3ROM = 0x0020;  // $C00A/0B
    static constexpr uint16_t MF_80COL     = 0x0040;  // $C00C/0D
    static constexpr uint16_t MF_ALTCHAR   = 0x0080;  // $C00E/0F

    // CPU pacing hook — EmulationController calls this with the cycle
    // count returned by M6502::run() so the paddle RC discharge timer
    // ticks against the real CPU clock instead of wallclock. Forwards to
    // the cassette device too (so its pulse advance stays cycle-aligned).
    void advanceCycles(int cycles);
    uint64_t getCycleCounter() const { return cycleCounter; }

private:
    std::array<uint8_t, 0x10000> mem{};       // flat 64 KB RAM/ROM mirror
    std::array<bool,    0x10000> writable{};  // false in ROM regions
    // Apple II/II+ 16 KB Language Card. $D000-$DFFF has two 4 KB banks;
    // $E000-$FFFF is one shared 8 KB bank. Together with base 48 KB RAM
    // this gives the II+ its ProDOS-required 64 KB.
    std::array<uint8_t, 0x1000> lcBank1{};
    std::array<uint8_t, 0x1000> lcBank2{};
    std::array<uint8_t, 0x2000> lcHigh{};
    // IIe extension. Allocated unconditionally (small) but only consulted
    // by the dispatcher when iieMode is true.
    std::array<uint8_t, 0x10000> aux{};       // auxiliary 64 KB (= RamWorks bank 0)
    std::array<uint8_t, 0x1000>  internalIORom{}; // motherboard $C000-$CFFF
    std::array<uint8_t, 0x1000>  auxLcBank1{};
    std::array<uint8_t, 0x1000>  auxLcBank2{};
    std::array<uint8_t, 0x2000>  auxLcHigh{};

    // RamWorks III backing store. The four `aux*` arrays above are the
    // "currently visible" bank — kept at fixed addresses so Apple2Display
    // can cache the auxData() pointer once. Switching banks memcpys the
    // visible buffers into `ramWorksBacking_[prev]` and the new bank out
    // of `ramWorksBacking_[curr]`. Stride per bank = 64K + 4K + 4K + 8K =
    // 80K. `ramWorksBanks_ == 1` disables the swap path entirely (stock
    // IIe). Layout per slot in backing: [0..0xFFFF]=aux,
    // [0x10000..0x10FFF]=auxLcBank1, [0x11000..0x11FFF]=auxLcBank2,
    // [0x12000..0x13FFF]=auxLcHigh. Total: ramWorksBanks_ × 0x14000.
    static constexpr uint32_t kRamWorksMaxBanks  = 128;        // MAME 8 MB cap
    static constexpr size_t   kRamWorksBankStride = 0x14000;   // 80 KB per bank
    std::vector<uint8_t> ramWorksBacking_;
    uint32_t ramWorksBanks_ = 1;   // 1 = stock 64 KB aux (no RamWorks)
    uint8_t  ramWorksBank_  = 0;   // current bank (MAME m_bank / 0x10000)
    void ramWorksSwapToBank(uint8_t newBank);  // memcpy in/out
    std::vector<uint8_t> characterRom;        // 2048 bytes once loaded
    std::string lastError;

    // Soft-switch state, guarded by stateMutex. Reads from the UI thread
    // are infrequent (once per frame, in render()).
    mutable std::mutex stateMutex;
    DisplayState display;

    // Keyboard.
    mutable std::mutex kbMutex;
    uint8_t lastKey   = 0;
    bool    keyReady  = false;
    // Paste queue — drains one byte into `lastKey` each time the CPU
    // clears the strobe via $C010. See pasteText().
    std::deque<uint8_t> pasteQueue;

    // Speaker.
    std::atomic<uint64_t> speakerToggles{0};
    SpeakerCallback speakerCb = nullptr;
    void*           speakerUser = nullptr;

    // Paddles + buttons. Paddle "value" is the discharge time of an RC
    // network — Apple II reads $C064-$C067 and counts cycles until the
    // register flips. We model that by remembering when the latch was armed.
    std::array<uint8_t, 4> paddleValue{ 128, 128, 128, 128 };
    std::array<bool,    3> paddleButton{ false, false, false };
    // IIe modifier keys, wired alongside the gameport buttons at
    // $C061-$C063. Atomic because the GLFW key callback runs on the
    // UI thread while the CPU worker polls them.
    std::atomic<bool> openAppleKey  { false };
    std::atomic<bool> solidAppleKey { false };
    std::atomic<bool> shiftKey      { false };
    // Init the latch "deep in the past" so the elapsed test
    // `(cycleCounter - paddleLatchCycle) >= paddleValue*11` is true at
    // boot regardless of paddleValue (max threshold = 255*11 = 2805).
    // Matches MAME apple2.cpp:259 `m_joystick_x1_time = 0` semantics
    // (timer already expired at startup). Without this, $C064-$C067
    // would return 0x80 for ~1408 cycles after every power-on and
    // confuse the //e auto-test that detects "no paddle connected".
    // Concretely: 0 - paddleLatchCycle in uint64 wraps to 0x10000 here,
    // which is well above any plausible threshold.
    uint64_t paddleLatchCycle = ~uint64_t(0) - 0xFFFFu;
    uint64_t cycleCounter     = 0;       // hand-rolled, see advanceCycles()

    // Cassette: non-owning pointer set by EmulationController.
    CassetteDevice* cassette = nullptr;

    // Speaker + CPU back-pointers for $C030 sub-instruction timestamping.
    SpeakerDevice* speaker = nullptr;
    M6502*         cpu     = nullptr;

    // //c / //c+ on-board IWM controller (non-owning). Mirrors $C0E0-
    // $C0EF accesses for the state machine; see `setIWM`. Lives in
    // EmulationController, attached/detached around profile switches.
    pom2::IWMDevice* iwmDevice = nullptr;
    // Default true: the IWM is authoritative on iicHasAltBank
    // profiles — `$C0EC/ED/EE/EF` reads return what the MAME-faithful
    // state machine produced from POM2's DiskImage flux stream (scaled
    // from LSS-cycle space at `DiskIICard::lssCycle = cpuCycleTotal*2`,
    // see `IWMDevice::nextTransition`). DiskIICard's LSS still
    // observes every access so motor sound / disk-turbo / head-step
    // tracking stay correct. Flip off via `setIWMAuthoritative(false)`
    // or `POM2_IWM_AUTHORITATIVE=0` env var to A/B compare against
    // the slot-bus path during regression bisect.
    bool             iwmAuthoritative = true;

    // Expansion bus — owns plugged cards.
    SlotBus slots;

    // Klaus harness flat-RAM mode. See setTestMode().
    bool testMode = false;

    // Language Card latch state. Reset default is ROM visible, writes
    // protected. Write-enable follows the real prewrite rule: an odd
    // $C08x switch must be accessed twice consecutively before RAM writes
    // to $D000-$FFFF are accepted.
    bool lcReadRam      = false;
    bool lcWriteEnable  = false;
    bool lcBank2Active  = true;
    bool lcPrewrite     = false;

    bool     iieMode      = false;
    uint16_t iieMemMode   = 0;       // OR of MF_* flags

    // //e auto-INTCXROM for the $C800-$CFFF shared expansion window.
    // Separate from the MF_INTCXROM softswitch ($C006/$C007/$C015): real
    // //e hardware sets this flip-flop when a read hits $C300-$C3FF with
    // SLOTC3ROM=off (handing the //e 80-col firmware control over the
    // 2 KB expansion ROM page so its $C800+ continuation routines run),
    // and clears it when a read hits $CFFF (same mechanism that releases
    // the slot expansion-ROM owner). Without this, JSR $C300 lands in the
    // 80-col firmware OK, but the firmware's JMP $C803 / $C87C / $C9B4 /
    // JSR $CD5B reach the slot bus (= $FF empty) and the CPU walks
    // forward through $FF (= BBS7 zp,rel, 3-byte) until it hits ROM data
    // it decodes as JMP indirect through a stale user-RAM vector — BRK
    // in zero RAM, monitor `*` prompt. Verbatim port of MAME
    // `apple2e.cpp:apple2e_state::c300_int_r` / `c800_int_r`. Pinned by
    // `iie_c8xx_smoke_test.cpp`.
    bool intC8Rom = false;

    // Apple //c ROMBANK ($C028 soft switch). When a 32 KB //c-style dump
    // is loaded, the second 16 KB firmware bank is stashed in
    // `iicAltFirmware`. Reads of $C028 toggle `iicRomBank`; when bank 1
    // is selected, $C100-$CFFF (under INTCXROM) and $D000-$FFFF (ROM
    // mode) dispatch to the alt firmware bytes instead of the regular
    // `internalIORom`/`mem` paths. `iicHasAltBank` gates the whole
    // mechanism — on II/II+/IIe profiles the $C028 access stays in the
    // cassette-toggle path (which is what those machines actually do).
    std::array<uint8_t, 0x4000> iicAltFirmware{};
    bool iicHasAltBank = false;
    bool iicRomBank    = false;   // false = bank 0 (cold-start), true = bank 1

    // Apple //c+ MIG (Multi-drive Interface Glue, MAME `apple2e.cpp:451`
    // + `apple2e.cpp:532-624 mig_r/mig_w`). Custom Apple gate-array that
    // bridges the on-board IWM to the //c+'s integrated 5.25" + 3.5"
    // drives. The //c+ alt firmware (bank 1) hits two MIG windows in the
    // expansion ROM area when ROMSWITCH is on:
    //   $CC00-$CCFF  →  mig offset 0x000-0x0FF  (drive control)
    //   $CE00-$CEFF  →  mig offset 0x200-0x2FF  (MIG RAM + head select)
    // Sub-decode (MAME lines 532-624):
    //   off  $40       write  → IWM reset
    //   off $80-$9F    write  → enable internal drive
    //   off $C0-$DF    write  → disable internal drive
    //   off $200-$21F  R/W    → migRam[migPage + (off & 0x1F)]
    //   off $220-$23F  R/W    → same, then migPage += 0x20 (mod 0x800)
    //   off $240-$25F  R/W    → clear 3.5" head select
    //   off $260-$27F  R/W    → set 3.5" head select
    //   off $2A0        R/W   → reset migPage = 0
    //   anything else  read   → floating bus
    //
    // The 2 KB migRam holds //c+ accelerator/disk-geometry state the
    // firmware caches across resets. MAME `apple2e.cpp:1700-1703` resets
    // it when ROMSWITCH transitions off; POM2 does the equivalent in
    // resetSoftSwitches() when iicRomBank flips back to 0. The 3.5"
    // side (hdsel + intdrive routing) is captured here even though POM2
    // doesn't model a 3.5" SmartPort drive — keeping the state lets the
    // //c+ alt firmware's probes complete without spinning.
    std::array<uint8_t, 0x800> migRam{};
    uint16_t                   migPage    = 0;
    bool                       migIntDrive = false;
    bool                       migHdSel    = false;

    /// MIG read at offset 0x000-0x2FF. CPU $CC00 = migOffset 0; CPU $CE00
    /// = migOffset 0x200 (the $CD00 gap is plain ROM).
    uint8_t migRead (uint16_t migOffset);
    void    migWrite(uint16_t migOffset, uint8_t value);

    // VBL (vertical-blank) state. Apple II frame = 262 NTSC scanlines
    // × 65 CPU cycles = 17030 cycles (the long-cycle stretch is not
    // modelled here; nominal 17046 cycles/frame is close enough).
    // Visible video = scanlines 0..191; vertical blank = 192..261.
    // `vblIrqMask` (IIe only) gates the IRQ. The pending flag is set
    // when entering VBL with mask on; cleared on $C019 read or $C05A
    // (disable) write. Asserted on the CPU IRQ line via cpu->setIRQ.
    bool vblIrqMask    = false;
    bool vblIrqPending = false;
    bool vblWasActive  = true;       // tracks transition into VBL window

    // Annunciator output state. AN0..AN3 are toggled by paired soft
    // switches ($C058/9 = AN0, $C05A/B = AN1, $C05C/D = AN2,
    // $C05E/F = AN3 — AN3 lives in `display.an3` because it also
    // drives Le Chat Mauve's FIFO clock). POM2 doesn't currently
    // forward AN0/1/2 to any external sink; the state is tracked so
    // a future GameI/O-style pin model can pick it up without
    // restructuring the soft-switch handler.
    bool an0 = false;
    bool an1 = false;
    bool an2 = false;

    void markRomRegion(uint16_t lo, uint16_t hi);
    uint8_t softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal);
    uint8_t languageCardSwitchAccess(uint16_t addr, bool isWrite);
    uint8_t languageCardRead(uint16_t addr) const;
    void    languageCardWrite(uint16_t addr, uint8_t value);

    /// "Floating bus" — the byte the video DMA is currently fetching.
    /// On a real Apple II, reading any soft switch that doesn't actively
    /// drive the data lines returns whichever byte the video circuit just
    /// latched off the DRAM. ProDOS / firmware code uses this as a poor-
    /// man's RNG and as the implicit return value of LC bank-select
    /// triggers ($C080-$C08F) and IIe paging triggers ($C001-$C00F that
    /// don't otherwise return state). Approximates the address by
    /// converting `cycleCounter` into a (line, column) pair and applying
    /// the standard text/HGR row-interleave formulas.
    uint8_t floatingBus() const;

    // IIe-only routing helpers. Selected per address range based on the
    // current iieMemMode + DisplayState; see the table at the top of the
    // header for the rules.
    uint8_t iieMemRead(uint16_t addr);
    void    iieMemWrite(uint16_t addr, uint8_t value);
    void    iieHandleSoftSwitch(uint16_t addr);
    uint8_t iieReadStatus(uint16_t addr) const;
};

#endif // POM2_MEMORY_H
