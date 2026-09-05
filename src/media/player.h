// Threads: the audio callback owns clock and mix; transport calls are
// safe from any thread at any time.

#pragma once

#include <cstdint>
#include <memory>

#include "media/audio_mix.h"

namespace looks::media {

class Player {
public:
    // Export mixes at the same rate and channel pair as the monitor.
    static constexpr uint32_t kClockRate = 48000;
    static constexpr uint32_t kChannels = 2;

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // No-op when unchanged; frames == 0 parks the transport.
    void configure(double fps, uint32_t frames);
    bool active() const;

    // Holds the old state one swap longer; the audio callback must not
    // become the last owner of a PCM buffer.
    void set_mix(std::shared_ptr<const MixState> mix);

    void play();
    void pause();
    bool playing() const;
    void set_looping(bool loop);

    // Range 0..2. Gain never touches the clock; silence still advances.
    void set_gain(float gain);

    // No-op with an audio device. Callers must tick every cycle;
    // the fallback clock moves only when ticked.
    void tick();

    uint32_t frame_count() const;
    double fps() const;
    double duration_seconds() const;

    // [in, out) frame indices, clamped; playback and loop stay inside.
    void set_trim(uint32_t in_frame, uint32_t out_frame);
    uint32_t trim_in() const;
    uint32_t trim_out() const;

    // Wraps in [in, out) when out > in; 0/0 loops the whole trim.
    void set_loop_region(uint32_t in_frame, uint32_t out_frame);
    bool looping() const;
    // The effective wrap window [in, out): loop clamped to trim, else trim.
    void loop_bounds(uint32_t* in_frame, uint32_t* out_frame) const;

    // Positive delays monitored audio against video.
    void set_audio_offset(double seconds);

    void seek_frame(uint32_t frame_index);
    uint32_t current_frame_index() const;
    double position_seconds() const;

    uint32_t audio_channels() const;
    uint32_t audio_sample_rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace looks::media
