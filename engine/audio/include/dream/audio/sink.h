// Audio output.
//
// The AICA produces one stereo sample per 44.1 kHz tick of the guest's virtual clock. The host's
// sound device consumes samples on its own real clock and never waits. Those two clocks are not the
// same clock, and everything awkward about audio output follows from that: run the guest faster
// than real time and samples pile up, slower and the device runs dry.
//
// The launcher paces the guest against the wall clock in windowed mode, so the two rates agree to
// within the jitter of a frame; the sink queues samples to absorb that jitter and nudges the
// device's playback rate to keep the queue near its target.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dream::audio {

class Sink {
public:
    Sink() = default;
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    // Opens the default playback device as 16-bit stereo at `rate`. False with error() set when
    // there is no device, which is an ordinary outcome on a build machine and not a failure worth
    // stopping a run for.
    bool open(unsigned rate = 44100);
    void close();
    bool is_open() const noexcept { return device_ != nullptr; }

    // One stereo sample from the AICA. Buffered until a block is worth sending.
    void push(std::int16_t left, std::int16_t right);
    // Sends whatever is buffered, whether or not it fills a block. For the end of a run.
    void flush();

    // How many samples the device still has to play.
    unsigned queued_samples() const;

    // How full the queue is allowed to get before samples are dropped. Roughly a fifth of a second
    // at 44.1 kHz: enough to ride out a slow frame, short enough that sound stays in step with the
    // picture.
    unsigned high_water = 8192;

    // Dynamic rate control: the guest's clock and the sound card's never agree
    // exactly, so the queue drifts until it overflows (a dropped block, a click) or runs dry (an
    // underrun, a crackle). Each block nudges SDL's playback ratio by at most `max_skew` (0.5 %,
    // far below what anyone hears as pitch) to hold the queue at `target` samples. Dropping is
    // kept for the case rate control cannot fix, such as the guest having been paused.
    unsigned target = 3072;   // ~70 ms at 44.1 kHz: room for a slow frame, short enough for sync
    float max_skew = 0.005f;
    bool rate_control = true;

    float ratio = 1.0f;  // the playback ratio last set
    const std::string& error() const noexcept { return error_; }

private:
    static constexpr std::size_t kBlockFrames = 512;  // stereo frames per send

    // The playback device as each implementation keeps it: SDL3's audio stream (sdl_sink.cpp), or
    // for a NextUI build SDL2's device with a queue and resampler of its own (sdl2_sink.cpp).
    struct Device;
    Device* device_ = nullptr;
    std::vector<std::int16_t> block_;
    unsigned rate_ = 44100;
    std::string error_;
};

}  // namespace dream::audio
