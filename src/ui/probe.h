// UI probes: a per-frame registry of named widget rects (logical px)
// that the script host reads to click controls by NAME instead of by
// coordinates. Widgets register during their draw pass; the frame's
// accumulation swaps to the readable side at the next frame begin, so
// readers see the last fully-drawn frame (the same one-frame-stale
// contract as the app's other cached rects). Single-threaded: the UI
// thread owns both sides.

#pragma once

#include <string>
#include <vector>

#include "ui/types.h"

namespace looks::ui {

struct Probe {
    std::string name;
    Rect rect;
};

// Swap accumulation -> readable and start a fresh frame. Call once per
// frame, before any widget draws.
void probe_frame_begin();

// Register a rect under `name` during draw. Duplicate names are kept in
// draw order - probe_find picks by index.
void probe_add(const std::string& name, const Rect& rect);

// Lookup in the LAST completed frame. index picks among duplicates
// (draw order). False when absent.
bool probe_find(const std::string& name, int index, Rect* out);

// Every probe of the last completed frame, draw order.
const std::vector<Probe>& probe_list();

}  // namespace looks::ui
