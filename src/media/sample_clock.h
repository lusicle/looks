// Frame <-> audio-sample conversions: ONE rounding rule for every
// consumer. A frame's first sample is the first tick AT OR AFTER its
// start (ceil minus epsilon), so flooring a sample back to a frame
// lands on the same frame at any rate. Nearest-rounding here makes
// every frame-through-cursor round trip read the previous frame; ceil
// without the epsilon gates one sample late whenever frame/fps*rate
// lands a hair above an integer.
#pragma once

#include <cmath>
#include <cstdint>

namespace looks::media {

inline int64_t frame_to_sample(double frame, double fps, double rate) {
    return static_cast<int64_t>(std::ceil(frame / fps * rate - 1e-6));
}

// Seconds to a sample offset, half away from zero: negative nudges
// round symmetrically to positive ones.
inline int64_t seconds_to_samples(double seconds, double rate) {
    return static_cast<int64_t>(seconds * rate +
                                (seconds >= 0.0 ? 0.5 : -0.5));
}

}  // namespace looks::media
