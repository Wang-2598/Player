#include "Demuxer.h"
#include "Config.h"
#include <cstdio>
#include <algorithm>

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/avutil.h>
}

Demuxer::Demuxer()
    : video_queue_(Config::VIDEO_PACKET_QUEUE_SIZE)
    , audio_queue_(Config::AUDIO_PACKET_QUEUE_SIZE)
{}

Demuxer::~Demuxer() {
    stop();
}

bool Demuxer::open(const std::string& filename) {
    int ret = avformat_open_input(&fmt_ctx_, filename.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[Demuxer] Cannot open '%s': %s\n", filename.c_str(), errbuf);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[Demuxer] Cannot find stream info\n");
        return false;
    }

    // Find best video and audio streams.
    video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx_ < 0 && audio_stream_idx_ < 0) {
        fprintf(stderr, "[Demuxer] No audio or video stream found\n");
        return false;
    }

    if (fmt_ctx_->duration != AV_NOPTS_VALUE)
        duration_ = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;

    return true;
}

void Demuxer::start() {
    if (running_.load()) return;
    running_.store(true);
    eof_.store(false);
    thread_ = std::thread(&Demuxer::demuxLoop, this);
}

void Demuxer::stop() {
    running_.store(false);
    // Abort queues so that any blocked push/pop returns immediately.
    video_queue_.abort();
    audio_queue_.abort();
    if (thread_.joinable()) thread_.join();
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
}

void Demuxer::requestSeek(double seconds) {
    seconds = std::max(0.0, std::min(seconds, duration_));
    seek_target_.store(seconds);
    seek_requested_.store(true);
    // Wake queues in case push is blocked.
    video_queue_.flush();
    audio_queue_.flush();
    video_queue_.resetAbort();
    audio_queue_.resetAbort();
}

void Demuxer::flushQueues() {
    video_queue_.flush();
    audio_queue_.flush();
}

const AVStream* Demuxer::videoStream() const {
    if (video_stream_idx_ < 0 || !fmt_ctx_) return nullptr;
    return fmt_ctx_->streams[video_stream_idx_];
}

const AVStream* Demuxer::audioStream() const {
    if (audio_stream_idx_ < 0 || !fmt_ctx_) return nullptr;
    return fmt_ctx_->streams[audio_stream_idx_];
}

void Demuxer::demuxLoop() {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "[Demuxer] Failed to allocate packet\n");
        return;
    }

    while (running_.load()) {
        // Handle pending seek requests.
        if (seek_requested_.load()) {
            seek_requested_.store(false);
            double target = seek_target_.load();
            int64_t ts = static_cast<int64_t>(target * AV_TIME_BASE);
            // AVSEEK_FLAG_BACKWARD seeks to the nearest keyframe <= target.
            int ret = av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0)
                fprintf(stderr, "[Demuxer] Seek failed\n");
            eof_.store(false);
        }

        // Back off if queues are saturated to avoid burning CPU.
        if (video_queue_.size() >= Config::VIDEO_PACKET_QUEUE_SIZE - 4 &&
            audio_queue_.size() >= Config::AUDIO_PACKET_QUEUE_SIZE - 4) {
            SDL_Delay(5);
            continue;
        }

        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(fmt_ctx_->pb)) {
                eof_.store(true);
                // Push null packets to signal EOF to decoders.
                if (video_stream_idx_ >= 0) {
                    AVPacket* null_pkt = av_packet_alloc();
                    null_pkt->stream_index = video_stream_idx_;
                    video_queue_.push(null_pkt);
                }
                if (audio_stream_idx_ >= 0) {
                    AVPacket* null_pkt = av_packet_alloc();
                    null_pkt->stream_index = audio_stream_idx_;
                    audio_queue_.push(null_pkt);
                }
                // Wait until seek is requested or stop is called.
                while (running_.load() && !seek_requested_.load()) {
                    SDL_Delay(10);
                }
            } else {
                fprintf(stderr, "[Demuxer] av_read_frame error: %d\n", ret);
            }
            continue;
        }

        if (pkt->stream_index == video_stream_idx_) {
            AVPacket* p = av_packet_alloc();
            av_packet_move_ref(p, pkt);
            if (!video_queue_.push(p)) {
                // Aborted; packet already freed by push().
            }
        } else if (pkt->stream_index == audio_stream_idx_) {
            AVPacket* p = av_packet_alloc();
            av_packet_move_ref(p, pkt);
            if (!audio_queue_.push(p)) {
                // Aborted
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
}
