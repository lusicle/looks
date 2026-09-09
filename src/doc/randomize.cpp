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
    // These ids match in full: a substring match catches "gop" with "op".
    static const char* const kFrozenExact[] = {
        "op",     "channels", "counter", "pattern", "flip",
        "r_from", "g_from",   "b_from",  "font",    "drop_i",
        "output", "offset",   "target",  "type"};
    for (const char* f : kFrozenExact)
        if (std::strcmp(desc.id, f) == 0) return false;
    return true;
}

namespace {

void randomize_one(Document& doc, UndoStack& undo, uint64_t look,
                   uint64_t effect_id, float intensity,
                   uint64_t rng_seed) {
    // A stale id must not edit the fallback look: check before doc.look().
    const Look* guard = doc.find_look(look);
    if (!guard) return;
    const EffectInstance* effect = find_effect(*guard, effect_id);
    if (!effect) return;
    const EffectInstance& fx = *effect;
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
        undo.execute(doc, set_param_command(look, effect_id,
                                            static_cast<int>(p), next));
    }
}

}  // namespace

void randomize_effect(Document& doc, UndoStack& undo, uint64_t look,
                      uint64_t effect_id,
                      float intensity, uint64_t rng_seed) {
    undo.begin_group("Randomize Effect");
    randomize_one(doc, undo, look, effect_id, intensity,
                  rng_seed);
    undo.end_group();
}

void randomize_look(Document& doc, UndoStack& undo, uint64_t look,
                     float intensity, uint64_t rng_seed) {
    const Look* guard = doc.find_look(look);
    if (!guard) return;
    undo.begin_group("Randomize Look");
    const size_t n = doc.look(look).effects.size();
    for (size_t i = 0; i < n; ++i)
        randomize_one(doc, undo, look, guard->effects[i].id, intensity, rng_seed);
    undo.end_group();
}

}  // namespace looks::doc
