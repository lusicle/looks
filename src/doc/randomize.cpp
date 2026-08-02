#include "doc/randomize.h"

#include <cstring>

#include "doc/stack_commands.h"
#include "util/hash.h"

namespace looks::doc {

bool param_randomizable(const ParamDesc& desc) {
    static const char* const kFrozen[] = {"mode", "palette", "set", "corner",
                                          "preset", "shape"};
    for (const char* f : kFrozen)
        if (std::strstr(desc.id, f)) return false;
    // Exact-match selectors (substring would catch "gop" via "op"):
    // discrete identity knobs on the newer effects.
    static const char* const kFrozenExact[] = {
        "op",     "channels", "counter", "pattern", "flip",
        "r_from", "g_from",   "b_from",  "font",    "drop_i"};
    for (const char* f : kFrozenExact)
        if (std::strcmp(desc.id, f) == 0) return false;
    return true;
}

namespace {

void randomize_one(Document& doc, UndoStack& undo, size_t layer_index,
                   size_t effect_index, float intensity, uint64_t rng_seed) {
    const EffectInstance& fx = doc.layers[layer_index].stack[effect_index];
    const EffectInfo& info = effect_info(fx.type);
    for (uint32_t p = 0; p < info.param_count; ++p) {
        const ParamDesc& desc = info.params[p];
        if (!param_randomizable(desc)) continue;
        const float h = hash_float01(rng_seed, (fx.id << 8) | p);
        const float target =
            desc.min_value + h * (desc.max_value - desc.min_value);
        const float cur = fx.params[p];
        const float next = cur + (target - cur) * intensity;
        if (next == cur) continue;
        undo.execute(doc, set_param_command(layer_index, effect_index,
                                            static_cast<int>(p), next));
    }
}

}  // namespace

void randomize_effect(Document& doc, UndoStack& undo, size_t layer_index,
                      size_t effect_index, float intensity,
                      uint64_t rng_seed) {
    undo.begin_group("Randomize Effect");
    randomize_one(doc, undo, layer_index, effect_index, intensity, rng_seed);
    undo.end_group();
}

void randomize_stack(Document& doc, UndoStack& undo, size_t layer_index,
                     float intensity, uint64_t rng_seed) {
    undo.begin_group("Randomize Stack");
    const size_t n = doc.layers[layer_index].stack.size();
    for (size_t i = 0; i < n; ++i)
        randomize_one(doc, undo, layer_index, i, intensity, rng_seed);
    undo.end_group();
}

}  // namespace looks::doc
