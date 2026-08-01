// Preview player: mezzanine-only. A decode worker keeps a ring of
// decoded frames ahead of the playhead (CPU-side I420 — they reach the GPU
// as plain uploads in the engine milestone). The miniaudio output callback
// is the MASTER CLOCK: it advances the timeline sample cursor; video chases
// it. Sources without audio fall back to a wall-clock-stepped cursor with
// identical semantics. Instant seek; per-clip trim in/out; loop region.
//
// Threads: audio callback (clock), decode worker (owns its MezReader),
// callers (render/UI thread) via current_frame()/seek()/transport.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "codec/mez.h"

namespace looks::media {

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // pcm_path may be empty (silent clip). The PCM sidecar is loaded into
    // RAM (streaming lands with long-form support later).
    bool open(const std::filesystem::path& mez_path,
              const std::filesystem::path& pcm_path, std::string* error);
    void close();
    bool is_open() const;

    void play();
    void pause();
    bool playing() const;
    void set_looping(bool loop);

    // Monitor gain: 0 = mute, 1 = unity, up to 2. Never touches
    // the clock — silence still advances the timeline.
    void set_gain(float gain);
    float gain() const;

    // Advances the silent-clip fallback clock (no-op with an audio master
    // clock). Polling threads must call this even on cycles they skip —
    // the clock only moves when someone ticks it.
    void tick();

    // Frame-index timeline (deterministic: fixed-timestep on frame index).
    uint32_t frame_count() const;
    double fps() const;
    double duration_seconds() const;

    // Trim region [in, out) in frame indices; playback and loop stay inside.
    void set_trim(uint32_t in_frame, uint32_t out_frame);
    uint32_t trim_in() const;
    uint32_t trim_out() const;

    // Loop region: looping playback wraps inside [in, out) when
    // out > in (clamped to the trim); 0/0 loops the whole trim.
    void set_loop_region(uint32_t in_frame, uint32_t out_frame);

    // Audio nudge: positive delays monitored audio against video.
    void set_audio_offset(double seconds);

    void seek_frame(uint32_t frame_index);
    void seek_seconds(double seconds);
    uint32_t current_frame_index() const;
    double position_seconds() const;

    // Latest decoded frame at (or nearest below) the playhead. May briefly
    // return the previous frame right after a seek while the worker catches
    // up; never blocks.
    std::shared_ptr<const codec::DecodedFrame> current_frame();

    uint32_t audio_channels() const;
    uint32_t audio_sample_rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace looks::media
