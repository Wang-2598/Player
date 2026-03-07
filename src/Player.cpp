#include "Player.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/rational.h>
}

Player::Player() = default;

Player::~Player() {
    // Abort the demuxer packet queues first so that decode threads blocked
    // on pktQueue.pop() can unblock before we try to join them.
    if (demuxer_) {
        demuxer_->videoQueue().abort();
        demuxer_->audioQueue().abort();
    }
    if (adecoder_) adecoder_->stop();
    if (vdecoder_) vdecoder_->stop();
    if (demuxer_)  demuxer_->stop();
    if (audio_)    audio_->close();
}

bool Player::open(const Options& opts) {
    volume_ = opts.initVolume;

    // ----- Demuxer -----
    demuxer_ = std::make_unique<Demuxer>();
    if (!demuxer_->open(opts.filename)) return false;

    // ----- Audio output -----
    audio_ = std::make_unique<AudioOutput>();
    if (!audio_->init()) return false;
    audio_->setVolume(volume_);
    audio_->setClock(&clock_);
    audio_->setSpeed((float)speed_);

    // ----- Video decoder -----
    if (demuxer_->videoStreamIndex() >= 0) {
        vdecoder_ = std::make_unique<VideoDecoder>();
        if (!vdecoder_->init(demuxer_->videoStream(), opts.decodeMode)) {
            fprintf(stderr, "[Player] Video decoder init failed\n");
            vdecoder_.reset();
        } else {
            decode_mode_str_ = vdecoder_->isHardware() ? "NVDEC" : "Software";
        }
    }

    // ----- Audio decoder -----
    if (demuxer_->audioStreamIndex() >= 0) {
        adecoder_ = std::make_unique<AudioDecoder>();
        if (!adecoder_->init(demuxer_->audioStream())) {
            fprintf(stderr, "[Player] Audio decoder init failed\n");
            adecoder_.reset();
        }
    }

    // ----- Determine initial window size -----
    int initW = Config::DEFAULT_WIDTH;
    int initH = Config::DEFAULT_HEIGHT;
    if (vdecoder_ && vdecoder_->width() > 0) {
        initW = vdecoder_->width();
        initH = vdecoder_->height();
        // Cap to a reasonable maximum while preserving aspect ratio.
        if (initW > 1920) {
            initH = initH * 1920 / initW;
            initW = 1920;
        }
    }

    // ----- Video renderer -----
    renderer_ = std::make_unique<VideoRenderer>();
    if (!renderer_->init("Player", initW, initH)) return false;

    // ----- OSD -----
    osd_ = std::make_unique<OSD>(renderer_->renderer());

    // ----- Seed the clock -----
    clock_.seek(0.0);
    fps_tick_ns_ = SDL_GetTicksNS();

    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void Player::run() {
    // Start all background threads.
    demuxer_->start();
    if (vdecoder_) vdecoder_->start(demuxer_->videoQueue());
    if (adecoder_) adecoder_->start(demuxer_->audioQueue(), *audio_);

    SDL_Event event;

    while (!quit_) {
        // Process all pending SDL events.
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    quit_ = true;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    onKeyDown(event.key.scancode);
                    break;

                case SDL_EVENT_MOUSE_WHEEL:
                    onMouseWheel(event.wheel.y);
                    break;

                case SDL_EVENT_WINDOW_RESIZED:
                    onWindowResized(event.window.data1, event.window.data2);
                    break;

                default: break;
            }
        }

        // Render cycle.
        renderer_->clear();

        if (!paused_) {
            if (!tryRenderVideoFrame())
                renderer_->blitLastFrame();
        } else {
            renderer_->blitLastFrame();
        }

        updateOSD();
        osd_->render();

        renderer_->present();

        SDL_Delay(1); // yield CPU between frames
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void Player::onKeyDown(SDL_Scancode key) {
    switch (key) {
        case SDL_SCANCODE_ESCAPE:
            quit_ = true;
            break;

        case SDL_SCANCODE_SPACE:
            togglePause();
            break;

        case SDL_SCANCODE_RIGHT:
            seek(+Config::SEEK_STEP);
            break;

        case SDL_SCANCODE_LEFT:
            seek(-Config::SEEK_STEP);
            break;

        case SDL_SCANCODE_UP:
            setVolume(volume_ + (float)Config::VOLUME_STEP_KEY);
            break;

        case SDL_SCANCODE_DOWN:
            setVolume(volume_ - (float)Config::VOLUME_STEP_KEY);
            break;

        case SDL_SCANCODE_TAB:
            stepSpeedLevel(+1);
            break;

        case SDL_SCANCODE_LSHIFT:
            osd_->toggleVisible();
            break;

        default: break;
    }
}

void Player::onMouseWheel(float dy) {
    // Scrolling up (dy > 0) increases volume; scrolling down decreases it.
    setVolume(volume_ + (float)(dy * Config::VOLUME_STEP_WHEEL));
}

void Player::onWindowResized(int w, int h) {
    renderer_->onWindowResized(w, h);
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void Player::togglePause() {
    paused_ = !paused_;
    if (paused_) {
        clock_.pause();
        audio_->setPaused(true);
    } else {
        clock_.resume();
        audio_->setPaused(false);
    }
}

void Player::seek(double offsetSeconds) {
    double target = clock_.get() + offsetSeconds;
    target = std::max(0.0, target);
    if (demuxer_->duration() > 0)
        target = std::min(target, demuxer_->duration());

    // Request a seek from the demux thread; it will flush queues internally.
    demuxer_->requestSeek(target);

    // Flush decoder + filter state.
    if (vdecoder_) vdecoder_->flush();
    if (adecoder_) adecoder_->flush();

    // Clear buffered audio so the clock is not dragged behind the seek target.
    audio_->flush();
    // Discard audio frames before the seek target (keyframe gap) so they cannot
    // override the clock we are about to set.
    audio_->setMinPts(target);

    // Tell the video decoder to discard frames before the target internally,
    // bypassing frame_queue_ entirely — no vsync stalls during fast-forward.
    if (vdecoder_) vdecoder_->setSkipUntilPts(target);

    // Signal that the next rendered frame should anchor the clock rather than
    // be compared against it.  This prevents the clock overshoot that occurs
    // during av_seek_frame latency from causing all near-target frames to be
    // batch-dropped (which shows as "video frozen, OSD time increasing").
    seek_pending_ = (vdecoder_ != nullptr);

    // Snap the clock to the seek target so the video renderer accepts the
    // first decoded frame immediately.
    clock_.seek(target);
    last_video_pts_   = target;
    last_frame_delay_ = 0.040;

    // Re-open the queues for writing (they were aborted during requestSeek).
    demuxer_->videoQueue().resetAbort();
    demuxer_->audioQueue().resetAbort();
    if (vdecoder_) vdecoder_->frameQueue().resetAbort();
}

void Player::setVolume(float vol) {
    vol = std::clamp(vol, (float)Config::VOLUME_MIN, (float)Config::VOLUME_MAX);
    volume_ = vol;
    audio_->setVolume(vol);
}

void Player::setSpeed(double speed) {
    speed_ = speed;
    clock_.setSpeed(speed);
    if (adecoder_) adecoder_->setSpeed(speed); // sets speed_changed_ in audio thread
    audio_->setSpeed((float)speed);
    // Flush SDL buffer immediately so the new speed is heard right away.
    // AudioDecoder::processFrame() checks speed_changed_ inside the atempo
    // filter drain loop and exits early, so no old-speed audio is pushed to
    // SDL after this flush — eliminating the electric-buzz / crackling artifact.
    audio_->flush();
    fprintf(stdout, "[Player] Speed: %.2fx\n", speed);
}

void Player::stepSpeedLevel(int direction) {
    const auto& levels = Config::SPEED_LEVELS;
    speed_index_ += direction;
    if (speed_index_ < 0)
        speed_index_ = (int)levels.size() - 1; // wrap backward to fastest (3.0x)
    if (speed_index_ >= (int)levels.size())
        speed_index_ = 0; // wrap forward to slowest (0.25x)
    setSpeed(levels[speed_index_]);
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------

bool Player::tryRenderVideoFrame() {
    if (!vdecoder_) return false;

    FrameQueue& fq = vdecoder_->frameQueue();

    // ---- Seek anchor path ----
    // After a seek, clock_.seek(target) makes the clock free-run from target
    // in real time.  av_seek_frame + frame-skip latency can be >0.5 s, so
    // by the time the first post-seek frame arrives the clock has overshot
    // by clock_overshoot = elapsed, making diff = frame_pts - clock_.get()
    // very negative and triggering batch-drop → video freezes.
    //
    // Fix: render the first post-seek frame immediately (no sync check) and
    // anchor the clock to its actual PTS so future frames compare correctly.
    if (seek_pending_) {
        AVFrame* frame = fq.peek();
        if (!frame) return false; // skip-phase still running; wait

        int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp : frame->pts;
        if (ts != AV_NOPTS_VALUE) {
            double pts = ts * av_q2d(vdecoder_->timeBase());
            clock_.seek(pts);     // re-anchor clock to real decoded position
            last_video_pts_  = pts;
            last_frame_delay_ = 0.040;
        }
        seek_pending_ = false;

        frame = fq.pop();
        renderer_->renderFrame(frame);
        av_frame_free(&frame);
        return true;
    }

    // Drop all frames that are too far behind the audio clock in one batch,
    // so the decoder thread is unblocked quickly after a seek.
    while (true) {
        AVFrame* frame = fq.peek();
        if (!frame) return false;

        double pts = 0.0;
        int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp : frame->pts;
        if (ts != AV_NOPTS_VALUE)
            pts = ts * av_q2d(vdecoder_->timeBase());

        double diff = pts - clock_.get();

        if (diff >= -Config::FRAME_DROP_THRESHOLD)
            break; // this frame is either on time or early — stop dropping

        frame = fq.pop();
        av_frame_free(&frame);
    }

    AVFrame* frame = fq.peek();
    if (!frame) return false;

    // Determine the presentation time of this frame in seconds.
    double pts = 0.0;
    int64_t ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    ? frame->best_effort_timestamp : frame->pts;
    if (ts != AV_NOPTS_VALUE)
        pts = ts * av_q2d(vdecoder_->timeBase());

    double audio_clock    = clock_.get();
    double diff           = pts - audio_clock;
    double sync_threshold = std::clamp(last_frame_delay_,
                                       Config::AV_SYNC_THRESHOLD_MIN,
                                       Config::AV_SYNC_THRESHOLD_MAX);

    if (diff > sync_threshold) {
        // Frame is too early: wait for next cycle.
        return false;
    }

    // Consume and render the frame.
    frame = fq.pop();
    renderer_->renderFrame(frame);

    // Update frame timing estimate.
    if (last_video_pts_ > 0.0 && pts > last_video_pts_)
        last_frame_delay_ = pts - last_video_pts_;
    last_video_pts_ = pts;

    av_frame_free(&frame);

    // Measure display FPS.
    ++frame_count_;
    uint64_t now_ns = SDL_GetTicksNS();
    double elapsed  = (now_ns - fps_tick_ns_) * 1e-9;
    if (elapsed >= 1.0) {
        display_fps_ = frame_count_ / elapsed;
        frame_count_ = 0;
        fps_tick_ns_ = now_ns;
    }

    return true;
}

void Player::updateOSD() {
    if (!osd_->isVisible()) return;

    osd_->setStatus(paused_ ? "Paused" : "Playing");
    osd_->setCurrentTime(clock_.get());
    osd_->setDuration(demuxer_->duration());
    osd_->setFPS(display_fps_);
    osd_->setVolume(volume_);
    osd_->setSpeed(speed_);
    osd_->setDecodeMode(decode_mode_str_);

    if (vdecoder_) {
        osd_->setVideoCodec(vdecoder_->codecName());
        osd_->setResolution(vdecoder_->width(), vdecoder_->height());
        const AVStream* vs = demuxer_->videoStream();
        if (vs) osd_->setVideoBitrate(vs->codecpar->bit_rate);
    }

    if (adecoder_) {
        osd_->setAudioCodec(adecoder_->codecName());
    }
}
