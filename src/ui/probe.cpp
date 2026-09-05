#include "ui/probe.h"

#include <utility>

namespace looks::ui {

namespace {

std::vector<Probe>& accum() {
    static std::vector<Probe> v;
    return v;
}
std::vector<Probe>& readable() {
    static std::vector<Probe> v;
    return v;
}

}  // namespace

void probe_frame_begin() {
    readable().swap(accum());
    accum().clear();
}

void probe_add(std::string name, const Rect& rect) {
    accum().push_back({std::move(name), rect});
}

bool probe_find(const std::string& name, int index, Rect* out) {
    int seen = 0;
    for (const Probe& p : readable()) {
        if (p.name != name) continue;
        if (seen == index) {
            *out = p.rect;
            return true;
        }
        ++seen;
    }
    return false;
}

const std::vector<Probe>& probe_list() { return readable(); }

}  // namespace looks::ui
