// The first sample of a frame is the first tick at or after the frame start.
// The epsilon stops ceil from gating one sample too late on float error.
#pragma once

#include <cmath>
#include <cstdint>

namespace looks::media {

inline int64_t frame_to_sample(double frame, double fps, double rate) {
    return static_cast<int64_t>(std::ceil(frame / fps * rate - 1e-6));
}

// Round half away from zero so negative offsets mirror positive ones.
inline int64_t seconds_to_samples(double seconds, double rate) {
    return static_cast<int64_t>(seconds * rate +
                                (seconds >= 0.0 ? 0.5 : -0.5));
}

}  // namespace looks::media
