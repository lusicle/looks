// The UI thread owns both probe buffers, so there is no lock.

#pragma once

#include <string>
#include <vector>

#include "ui/types.h"

namespace looks::ui {

struct Probe {
    std::string name;
    Rect rect;
};

// Call once per frame, before any widget draws.
void probe_frame_begin();

// Duplicate names stay in draw order; probe_find picks one by index.
void probe_add(const std::string& name, const Rect& rect);

// Reads the last completed frame, not the frame in progress.
bool probe_find(const std::string& name, int index, Rect* out);

// Reads the last completed frame, in draw order.
const std::vector<Probe>& probe_list();

}  // namespace looks::ui
