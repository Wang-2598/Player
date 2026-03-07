#pragma once

#include "Clock.h"
#include "Config.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <mutex>

// AudioOutput wraps an SDL3 audio stream.
// Push decoded, resampled PCM (S16 stereo 44100 Hz) via pushData().
// Volume and the master audio clock are managed here.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // Open SDL3 audio device. Returns false on failure.
    bool init();

    // Close device and free resources.
    void close();

    // Push interleaved S16LE stereo PCM data to the SDL3 audio stream.
    // pts     - media presentation time of the first sample in this chunk.
    // duration - duration covered by this chunk in seconds (media time).
    void pushData(const uint8_t* data, int byteCount, double pts, double duration);

    // Pause / resume the SDL audio device.
    void setPaused(bool paused);

    // Flush buffered audio data from the SDL stream (call on seek / speed change).
    void flush();

    // Set the minimum PTS threshold: audio frames ending before this time are
    // silently discarded and do NOT update the clock.  Call after flush() on seek
    // so that keyframe-to-target audio cannot drag the clock backwards.
    // Pass a negative value to disable (default).
    void setMinPts(double pts);

    // Volume: 0.0 = silent, 1.0 = full volume (SDL gain, >1.0 is allowed).
    void   setVolume(float vol);
    float  getVolume() const { return volume_.load(); }

    // Current playback position estimated from audio clock.
    double getClock() const;

    // Store a reference to the shared master clock.
    void setClock(Clock* clock) { clock_ = clock; }

    // Bytes per second for the current output format.
    int bytesPerSecond() const { return bytes_per_second_; }

    // Number of bytes currently buffered in the SDL audio stream.
    int queuedBytes() const;

    // The playback speed multiplier (needed for correct clock accounting).
    // Does NOT flush the SDL buffer; instead records how many bytes were
    // queued at the old speed so pushData() can compute the clock correctly
    // during the transition (avoids the crackling caused by SDL_ClearAudioStream).
    void  setSpeed(float speed);
    float getSpeed() const { return speed_.load(); }

private:
    SDL_AudioStream*  stream_           = nullptr;
    int               bytes_per_second_ = 0;
    std::atomic<float> volume_          {1.0f};
    std::atomic<float> speed_           {1.0f};
    Clock*            clock_            = nullptr;

    // The running end-PTS of all data pushed so far (media time).
    std::atomic<double> pushed_end_pts_ {0.0};
    // Audio frames ending before this PTS are discarded (set after seek).
    std::atomic<double> min_pts_        {-1.0};

    // Speed-transition accounting (avoid SDL buffer flush on speed change).
    // When speed changes, bytes already in the SDL buffer were pushed at the
    // old speed.  SDL consumes the buffer FIFO, so we track:
    //   trans_bytes_  - bytes queued at the old speed at the moment of change
    //   trans_speed_  - the old speed those bytes belong to
    //   new_pushed_   - bytes pushed since the last speed change
    // Then: old_remaining = max(0, total_queued - new_pushed_)
    //        new_remaining = total_queued - old_remaining
    // buffered_seconds = old_remaining/bps * trans_speed_
    //                  + new_remaining/bps * speed_
    std::atomic<int64_t> trans_bytes_  {0};
    std::atomic<float>   trans_speed_  {1.0f};
    std::atomic<int64_t> new_pushed_   {0};
};
