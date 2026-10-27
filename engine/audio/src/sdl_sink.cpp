// See sink.h.
#include "dream/audio/sink.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dream::audio {

struct Sink::Device {
    SDL_AudioStream* stream;
};

Sink::~Sink() {
    close();
}

bool Sink::open(unsigned rate) {
    close();
    rate_ = rate;
    // SDL_Init is safe to call again when the window has already initialised the video subsystem;
    // each subsystem is counted separately.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_ = std::string("SDL_InitSubSystem(audio): ") + SDL_GetError();
        return false;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(rate);
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        error_ = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        return false;
    }
    device_ = new Device{stream};
    // A device opened this way starts paused, so that an application can queue something before
    // the first sample is due.
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        error_ = std::string("SDL_ResumeAudioStreamDevice: ") + SDL_GetError();
        close();
        return false;
    }
    block_.reserve(kBlockFrames * 2);
    return true;
}

void Sink::close() {
    if (device_) {
        SDL_DestroyAudioStream(device_->stream);
        delete device_;
        device_ = nullptr;
    }
    block_.clear();
}

unsigned Sink::queued_samples() const {
    if (!device_)
        return 0;
    const int bytes = SDL_GetAudioStreamQueued(device_->stream);
    if (bytes <= 0)
        return 0;
    return static_cast<unsigned>(bytes) / 4u;  // 16-bit stereo
}

void Sink::push(std::int16_t left, std::int16_t right) {
    if (!device_)
        return;
    block_.push_back(left);
    block_.push_back(right);
    if (block_.size() < kBlockFrames * 2)
        return;
    const unsigned queued = queued_samples();
    if (rate_control && target > 0) {
        // Proportional control on the queue error, clamped. Above target: play slightly faster.
        const float error = (static_cast<float>(queued) - static_cast<float>(target)) /
                            static_cast<float>(target);
        float r = 1.0f + std::clamp(error * max_skew, -max_skew, max_skew);
        if (std::abs(r - ratio) > 0.0002f) {
            SDL_SetAudioStreamFrequencyRatio(device_->stream, r);
            ratio = r;
        }
    }
    // The guest is far ahead of the device: throw this block away rather than let the delay
    // between what is on screen and what is heard grow without limit. Dropping is audible once,
    // whereas a queue that only grows is audible for the rest of the run.
    if (queued > high_water) {
        block_.clear();
        return;
    }
    flush();
}

void Sink::flush() {
    if (!device_ || block_.empty())
        return;
    SDL_PutAudioStreamData(device_->stream, block_.data(),
                           static_cast<int>(block_.size() * sizeof(std::int16_t)));
    block_.clear();
}

}  // namespace dream::audio
