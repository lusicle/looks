// Transport: the TIMELINE's master clock (docs/look.md phase 4).
//
// This used to be a clip player - it owned a mezzanine reader, a decode
// ring, and a clock driven by one clip's PCM. None of that survives the
// look model: a look tree has many clips playing at once (decode_pool.h
// serves their frames) and its audio is a mix over the whole instance tree
// (audio_mix.h). What is left here is the part that was always a transport:
// a frame-indexed position over the PROJECT's length, advanced by the audio
// device callback, with the mix pulled through it.
//
// The clock no longer belongs to a clip, so a project with no audio, no
// media, or several clips all behave the same way. When no audio device
// opens, a wall-clock fallback steps the same cursor with identical
// semantics - callers tick() it and cannot tell the difference.
//
// Threads: audio callback (clock + mix), callers (render/UI thread) via
// the transport calls, all of which are safe to call at any time.

#pragma once

#include <cstdint>
#include <memory>

#include "media/audio_mix.h"

namespace looks::media {

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // The timeline being played: its length in frames and the project
    // frame rate. Safe to call every frame - a no-op when unchanged.
    // frames == 0 parks the transport (nothing to play).
    void configure(double fps, uint32_t frames);
    bool active() const;

    // Publishes the mix the monitor pulls from. The previous state is held
    // one swap longer so the audio callback can never be the last owner of
    // a PCM buffer (freeing megabytes in the callback is a dropout).
    void set_mix(std::shared_ptr<const MixState> mix);

    void play();
    void pause();
    bool playing() const;
    void set_looping(bool loop);

    // Monitor gain: 0 = mute, 1 = unity, up to 2. Never touches the
    // clock - silence still advances the timeline.
    void set_gain(float gain);
    float gain() const;

    // Advances the wall-clock fallback (no-op with an audio device).
    // Polling threads must call this even on cycles they skip - the clock
    // only moves when someone ticks it.
    void tick();

    // Frame-index timeline (deterministic: fixed-timestep on frame index).
    uint32_t frame_count() const;
    double fps() const;
    double duration_seconds() const;

    // Trim region [in, out) in frame indices; playback and loop stay
    // inside. Clamped to the configured length.
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

    uint32_t audio_channels() const;
    uint32_t audio_sample_rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace looks::media
