#include "AudioOutput.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    close();
}

bool AudioOutput::init() {
    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_S16;
    spec.channels = Config::AUDIO_CHANNELS;
    spec.freq     = Config::AUDIO_SAMPLE_RATE;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        fprintf(stderr, "[AudioOutput] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }

    // bytes_per_second: sample_rate * channels * bytes_per_sample (S16 = 2 bytes)
    bytes_per_second_ = Config::AUDIO_SAMPLE_RATE * Config::AUDIO_CHANNELS * 2;

    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioOutput::close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

void AudioOutput::pushData(const uint8_t* data, int byteCount, double pts, double duration) {
    if (!stream_ || byteCount <= 0) return;

    double end_pts = pts + duration;

    // Discard audio frames that end before the seek target.
    double min_pts = min_pts_.load();
    if (min_pts >= 0.0) {
        if (end_pts < min_pts) return; // discard pre-seek audio silently
        min_pts_.store(-1.0);          // reached target — disable filter
    }

    SDL_PutAudioStreamData(stream_, data, byteCount);
    new_pushed_.fetch_add(byteCount);

    pushed_end_pts_.store(end_pts);

    // Update the master clock.
    // When a speed change has just occurred the SDL buffer contains a mix of
    // old-speed and new-speed bytes.  SDL drains FIFO, so the old-speed bytes
    // are consumed first.  We compute their share explicitly so the clock
    // estimate stays accurate without having to flush the SDL buffer (which
    // would cause an audible click / electric-buzz artifact).
    if (clock_) {
        int64_t total_queued  = static_cast<int64_t>(queuedBytes());
        int64_t np            = new_pushed_.load();
        float   ts            = trans_speed_.load();
        float   cs            = speed_.load();

        // Bytes that were in the buffer before the last speed change.
        // max(0, ...) because np can exceed total_queued once old bytes drain.
        int64_t old_remaining = std::max(int64_t(0), total_queued - np);
        int64_t new_remaining = total_queued - old_remaining;

        double buffered_seconds =
            (old_remaining / static_cast<double>(bytes_per_second_)) * ts +
            (new_remaining / static_cast<double>(bytes_per_second_)) * cs;

        clock_->set(end_pts - buffered_seconds);
    }
}

void AudioOutput::setPaused(bool paused) {
    if (!stream_) return;
    if (paused) SDL_PauseAudioStreamDevice(stream_);
    else        SDL_ResumeAudioStreamDevice(stream_);
}

void AudioOutput::flush() {
    if (!stream_) return;
    SDL_ClearAudioStream(stream_);
    pushed_end_pts_.store(0.0);
    min_pts_.store(-1.0);
    // Reset transition tracking; after a seek the buffer is clean.
    trans_bytes_.store(0);
    trans_speed_.store(speed_.load());
    new_pushed_.store(0);
}

void AudioOutput::setMinPts(double pts) {
    min_pts_.store(pts);
}

void AudioOutput::setSpeed(float speed) {
    float old_speed = speed_.load();
    if (std::fabsf(old_speed - speed) > 0.001f) {
        // Record how many bytes are currently in the SDL buffer at the old
        // speed so that pushData() can compute the clock correctly for the
        // mixed-speed window without flushing the buffer.
        trans_bytes_.store(static_cast<int64_t>(queuedBytes()));
        trans_speed_.store(old_speed);
        new_pushed_.store(0);
    }
    speed_.store(speed);
}

void AudioOutput::setVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    volume_.store(vol);
    if (stream_) SDL_SetAudioStreamGain(stream_, vol);
}

double AudioOutput::getClock() const {
    if (!clock_) return 0.0;
    return clock_->get();
}

int AudioOutput::queuedBytes() const {
    if (!stream_) return 0;
    return SDL_GetAudioStreamQueued(stream_);
}
