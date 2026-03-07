#pragma once

#include "Demuxer.h"
#include "VideoDecoder.h"
#include "AudioDecoder.h"
#include "VideoRenderer.h"
#include "AudioOutput.h"
#include "OSD.h"
#include "Clock.h"
#include "Config.h"
#include <string>
#include <memory>
#include <atomic>

// Player is the top-level component that owns all sub-systems and runs the
// SDL3 event/render loop on the calling (main) thread.
//
// Background threads:
//   - Demuxer thread      : reads compressed packets
//   - VideoDecoder thread : decodes video packets -> VideoFrameQueue
//   - AudioDecoder thread : decodes + resamples audio -> AudioOutput (SDL3)
//
// Main thread:
//   - SDL event loop (keyboard/mouse)
//   - Video render cycle (peek frame, AV-sync, upload, present)
//   - OSD overlay
class Player {
public:
    Player();
    ~Player();

    struct Options {
        std::string  filename;
        DecodeMode   decodeMode  = DecodeMode::Software;
        float        initVolume  = 0.8f;
    };

    // Open the file and initialise all sub-systems.
    bool open(const Options& opts);

    // Run the main loop (blocks until the user quits).
    void run();

private:
    // ----- Event handlers -----
    void onKeyDown(SDL_Scancode key);
    void onMouseWheel(float dy);
    void onWindowResized(int w, int h);

    // ----- Playback control -----
    void togglePause();
    void seek(double offsetSeconds);
    void setVolume(float vol);
    void setSpeed(double speed);
    void stepSpeedLevel(int direction); // +1 = faster, -1 = slower

    // ----- Render cycle -----
    // Try to show the next video frame if its PTS has arrived.
    // Returns true if a frame was rendered.
    bool tryRenderVideoFrame();

    // Update OSD data fields from the current player state.
    void updateOSD();

    // ----- Owned components -----
    std::unique_ptr<Demuxer>       demuxer_;
    std::unique_ptr<VideoDecoder>  vdecoder_;
    std::unique_ptr<AudioDecoder>  adecoder_;
    std::unique_ptr<VideoRenderer> renderer_;
    std::unique_ptr<AudioOutput>   audio_;
    std::unique_ptr<OSD>           osd_;
    Clock                          clock_;

    // ----- State -----
    bool   paused_       = false;
    bool   quit_         = false;
    float  volume_       = 0.8f;
    double speed_        = 1.0;
    int    speed_index_  = Config::DEFAULT_SPEED_INDEX;

    // Measured FPS
    double display_fps_   = 0.0;
    int    frame_count_   = 0;
    uint64_t fps_tick_ns_ = 0;

    // Last rendered video frame PTS (to compute delay for next frame).
    double last_video_pts_    = 0.0;
    double last_frame_delay_  = 0.040; // initial guess

    // Set to true after a seek; cleared after the first post-seek frame is
    // rendered.  While true, tryRenderVideoFrame() skips the AV-sync check
    // and anchors the clock to the actual decoded frame PTS so the clock
    // overshoot during av_seek_frame latency does not cause mass frame drops.
    bool seek_pending_ = false;

    // Decode mode string for OSD
    std::string decode_mode_str_;
};
