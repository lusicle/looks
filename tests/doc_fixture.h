#pragma once

#include "doc/document.h"

// A new project holds no look. Tests that need one start here.
inline looks::doc::Document doc_with_look() {
    looks::doc::Document d;
    looks::doc::Look look;
    look.id = d.next_effect_id++;
    look.name = "look 1";
    looks::doc::Source base;
    base.id = d.next_effect_id++;
    base.name = "layer 1";
    look.links.push_back({base.id, 0, 0});
    look.sources.push_back(std::move(base));
    d.looks.push_back(std::move(look));
    return d;
}

inline void connect_test_chain(looks::doc::Look& look, uint64_t source,
                               std::initializer_list<uint64_t> effects) {
    uint64_t previous = source;
    for (uint64_t id : effects) {
        look.links.push_back({previous, id, 0});
        previous = id;
    }
    look.links.push_back({previous, 0, 0});
}
