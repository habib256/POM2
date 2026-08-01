#pragma once
// Drain-cursor arithmetic between a printer byte source and the host
// ImageWriter. Header-only and dependency-free on purpose: this is the
// logic `MainWindow::pumpImageWriter()` runs every frame, and it is where
// a "reprint the whole spool" bug lived, so it has to be pinnable without
// an ImGui context.
//
// Three sources can feed the printer — PrinterCard, GrapplerCard, and the
// SuperSerialCard's TX printer tap (the //c's printer port) — but only one
// at a time, and the pump keeps ONE cursor. Everything hard about it is in
// the handover.

#include <cstddef>

namespace pom2 {

/// Where should the ImageWriter resume draining from this frame?
///
/// `consumed` is the cursor, counted in the *current* source's absolute
/// byte offsets. Two things invalidate it:
///
///   * **The source changed** — a different card is plugged, or the SSC
///     tap took over. Re-seat at that source's CURRENT total, **not at
///     0**. A spool can outlive its source status: the SSC tap's does
///     (nothing in the app clears it), so re-seating at 0 re-drained
///     everything it had ever spooled. One frame with "Feed ImageWriter
///     printer" unticked was enough to reprint an entire session on the
///     next frame — and on a //c, where slot 1 is the printer port and
///     slot 2 the modem port and both are SSCs, unticking slot 1 handed
///     the source to slot 2 at 0 and printed the whole modem transcript
///     onto paper. A card that was genuinely just plugged totals 0, so
///     the case this rule was originally written for still works — and
///     the pump runs from the very first frame (the ImageWriter is built
///     in the MainWindow constructor), so a card is always first seen
///     empty in practice.
///
///     Adopting also means bytes a source spooled *while it was not the
///     source* never print, which is the physically right answer: the
///     cable was out. Unticking the tap unplugs the printer; what the
///     guest sent meanwhile went to a port with nothing on the end of it.
///
///   * **The spool got shorter than the cursor** — the panel's "Clear
///     spool" ran behind our back, so restart from the top of what is
///     there now rather than going silently deaf.
///
/// Updates `source` in place and returns the offset to drain from.
inline size_t printerFeedCursor(const void*& source, size_t consumed,
                                const void* nowSource, size_t nowTotal)
{
    if (source != nowSource) {
        source = nowSource;
        return nowTotal;            // adopt the backlog, do not reprint it
    }
    return (nowTotal < consumed) ? 0 : consumed;
}

}  // namespace pom2
