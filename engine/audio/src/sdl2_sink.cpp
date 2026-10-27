// See sink.h. The NextUI build's playback through SDL2, which has no audio stream to queue into
// and no playback ratio to set. Samples go into our own queue, read by the device's callback
// through a linear resampler stepping `ratio` queued samples per sample played, so queueing,
// dropping and rate control match sdl_sink.cpp.
#include "dream/audio/sink.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace dream::audio {

struct Sink::Device {
    SDL_AudioDeviceID id = 0;
    std::mutex mutex;
    std::vector<std::int16_t> ring = std::vector<std::int16_t>(2 * 32768);  // stereo; ~0.74 s
    std::size_t head = 0, count = 0;  // stereo frames: the oldest queued, how many
    float pos = 0.0f;                 // between the frame at head and the next, 0 to 1
    float step = 1.0f;                // queued frames per frame played: the playback ratio
    std::int16_t last[2] = {0, 0};    // the last frame played, held through an underrun

    std::size_t capacity() const { return ring.size() / 2; }

    void put(const std::int16_t* s, std::size_t frames) {
        for (std::size_t i = 0; i < frames && count < capacity(); ++i) {
            const std::size_t at = (head + count) % capacity();
            ring[2 * at] = s[2 * i];
            ring[2 * at + 1] = s[2 * i + 1];
            ++count;
        }
    }

    // The device's thread: `len` bytes of 16-bit stereo, interpolated between queued frames.
    static void callback(void* user, Uint8* stream, int len) {
        auto& d = *static_cast<Device*>(user);
        auto* out = reinterpret_cast<std::int16_t*>(stream);
        const int frames = len / 4;
        std::lock_guard<std::mutex> lock(d.mutex);
        const std::size_t cap = d.capacity();
        for (int i = 0; i < frames; ++i) {
            if (d.count >= 2) {
                const std::size_t a = d.head, b = (d.head + 1) % cap;
                for (int ch = 0; ch < 2; ++ch) {
                    const float x = static_cast<float>(d.ring[2 * a + ch]);
                    const float y = static_cast<float>(d.ring[2 * b + ch]);
                    d.last[ch] = static_cast<std::int16_t>(std::lround(x + (y - x) * d.pos));
                }
                d.pos += d.step;
                while (d.pos >= 1.0f && d.count >= 2) {
                    d.pos -= 1.0f;
                    d.head = (d.head + 1) % cap;
                    --d.count;
                }
            }
            out[2 * i] = d.last[0];
            out[2 * i + 1] = d.last[1];
        }
    }
};

Sink::~Sink() {
    close();
}

bool Sink::open(unsigned rate) {
    close();
    rate_ = rate;
    // Counted per subsystem: the disc screens' video is started and stopped apart from this.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        error_ = std::string("SDL_InitSubSystem(audio): ") + SDL_GetError();
        return false;
    }
    auto* d = new Device;
    SDL_AudioSpec want{}, have{};
    want.freq = static_cast<int>(rate);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &Device::callback;
    want.userdata = d;
    d->id = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts to the device's format
    if (!d->id) {
        error_ = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        delete d;
        return false;
    }
    device_ = d;
    SDL_PauseAudioDevice(d->id, 0);
    block_.reserve(kBlockFrames * 2);
    return true;
}

void Sink::close() {
    if (device_) {
        SDL_CloseAudioDevice(device_->id);  // the callback has stopped when this returns
        delete device_;
        device_ = nullptr;
    }
    block_.clear();
}

unsigned Sink::queued_samples() const {
    if (!device_)
        return 0;
    std::lock_guard<std::mutex> lock(device_->mutex);
    return static_cast<unsigned>(device_->count);
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
        const float r = 1.0f + std::clamp(error * max_skew, -max_skew, max_skew);
        if (std::abs(r - ratio) > 0.0002f) {
            std::lock_guard<std::mutex> lock(device_->mutex);
            device_->step = r;
            ratio = r;
        }
    }
    // The guest is far ahead of the device: throw this block away rather than let the delay
    // between what is on screen and what is heard grow without limit.
    if (queued > high_water) {
        block_.clear();
        return;
    }
    flush();
}

void Sink::flush() {
    if (!device_ || block_.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(device_->mutex);
        device_->put(block_.data(), block_.size() / 2);
    }
    block_.clear();
}

}  // namespace dream::audio
