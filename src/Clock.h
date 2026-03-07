#pragma once

#include <SDL3/SDL.h>
#include <atomic>
#include <mutex>

// Tracks the current playback position (in seconds) for AV synchronisation.
//
// The audio output thread calls set() after each audio frame is pushed to
// SDL so the video renderer can read get() and decide whether to show,
// skip, or wait for a video frame.
//
// The clock value represents the media time currently being played, NOT the
// total elapsed wall-clock time.
class Clock {
public:
    Clock() : pts_(0.0), timestamp_ns_(0), speed_(1.0), paused_(false), paused_pts_(0.0) {}

    // Set the current media time and record the wall-clock moment.
    void set(double pts) {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = pts;
        timestamp_ns_ = SDL_GetTicksNS();
    }

    // Read the current media time, extrapolating from the last set() call.
    double get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_) return paused_pts_;
        double elapsed = static_cast<double>(SDL_GetTicksNS() - timestamp_ns_) * 1e-9;
        return pts_ + elapsed * speed_;
    }

    // Freeze the clock at the current position.
    void pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_pts_ = pts_ + static_cast<double>(SDL_GetTicksNS() - timestamp_ns_) * 1e-9 * speed_;
        paused_ = true;
    }

    // Resume advancing from the frozen position.
    void resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = paused_pts_;
        timestamp_ns_ = SDL_GetTicksNS();
        paused_       = false;
    }

    // Jump to a specific media time (used after seeking).
    void seek(double pts) {
        std::lock_guard<std::mutex> lock(mutex_);
        pts_          = pts;
        paused_pts_   = pts;
        timestamp_ns_ = SDL_GetTicksNS();
    }

    // Update the playback speed multiplier without disrupting the current position.
    void setSpeed(double speed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!paused_) {
            // Recalibrate so that get() returns the same value after the change.
            double now = pts_ + static_cast<double>(SDL_GetTicksNS() - timestamp_ns_) * 1e-9 * speed_;
            pts_          = now;
            timestamp_ns_ = SDL_GetTicksNS();
        }
        speed_ = speed;
    }

    double getSpeed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return speed_;
    }

    bool isPaused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return paused_;
    }

private:
    mutable std::mutex mutex_;
    double             pts_;          // Media time at last set()
    uint64_t           timestamp_ns_; // SDL tick (ns) at last set()
    double             speed_;        // Playback speed multiplier
    bool               paused_;
    double             paused_pts_;   // Frozen media time while paused
};
