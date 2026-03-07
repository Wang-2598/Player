#include "AudioDecoder.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

extern "C" {
#include <libavutil/samplefmt.h>
#include <libavutil/avstring.h>
}

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() {
    stop();
}

bool AudioDecoder::init(const AVStream* stream) {
    if (!stream) return false;
    time_base_ = stream->time_base;

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[AudioDecoder] No decoder for codec id %d\n", stream->codecpar->codec_id);
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        fprintf(stderr, "[AudioDecoder] Failed to copy codec params\n");
        return false;
    }

    codec_ctx_->thread_count = 1;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        fprintf(stderr, "[AudioDecoder] avcodec_open2 failed\n");
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    fprintf(stdout, "[AudioDecoder] Using codec: %s\n", codec->long_name);
    return true;
}

void AudioDecoder::start(PacketQueue& pktQueue, AudioOutput& output) {
    if (running_.load()) return;
    running_.store(true);
    flush_requested_.store(false);
    thread_ = std::thread(&AudioDecoder::decodeLoop, this, std::ref(pktQueue), std::ref(output));
}

void AudioDecoder::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    destroyFilterGraph();
    if (swr_ctx_)  { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
    if (codec_ctx_){ avcodec_free_context(&codec_ctx_); }
}

void AudioDecoder::flush() {
    flush_requested_.store(true);
}

void AudioDecoder::setSpeed(double speed) {
    speed_.store(speed);
    speed_changed_.store(true);
    // The actual filter graph rebuild happens in the audio thread on the
    // next decoded frame, to avoid concurrent access to the filter graph.
}

const char* AudioDecoder::codecName() const {
    if (codec_ctx_ && codec_ctx_->codec)
        return codec_ctx_->codec->name;
    return "unknown";
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void AudioDecoder::destroyFilterGraph() {
    if (filter_graph_) {
        avfilter_graph_free(&filter_graph_);
        filter_graph_ = nullptr;
        filter_src_   = nullptr;
        filter_sink_  = nullptr;
    }
}

bool AudioDecoder::buildFilterGraph(const AVFrame* ref) {
    destroyFilterGraph();

    double speed = speed_.load();
    if (std::fabs(speed - 1.0) < 1e-4)
        return true; // No filter needed at 1x speed.

    filter_graph_ = avfilter_graph_alloc();
    if (!filter_graph_) return false;

    // Describe the input channel layout as a string (robust for all layouts).
    char chlayout_str[64];
    av_channel_layout_describe(&ref->ch_layout, chlayout_str, sizeof(chlayout_str));

    char args[256];
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d",
             ref->sample_rate,
             av_get_sample_fmt_name((AVSampleFormat)ref->format),
             chlayout_str,
             ref->sample_rate);

    const AVFilter* abuffer     = avfilter_get_by_name("abuffer");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !abuffersink) {
        fprintf(stderr, "[AudioDecoder] abuffer/abuffersink filter not found\n");
        destroyFilterGraph();
        return false;
    }

    if (avfilter_graph_create_filter(&filter_src_, abuffer, "in",
                                     args, nullptr, filter_graph_) < 0) {
        fprintf(stderr, "[AudioDecoder] Cannot create abuffer filter\n");
        destroyFilterGraph();
        return false;
    }

    if (avfilter_graph_create_filter(&filter_sink_, abuffersink, "out",
                                     nullptr, nullptr, filter_graph_) < 0) {
        fprintf(stderr, "[AudioDecoder] Cannot create abuffersink filter\n");
        destroyFilterGraph();
        return false;
    }

    // Build the atempo filter string.
    // atempo accepts [0.5, 100]; for 0.25x use two cascaded atempo=0.5 stages.
    std::string filter_desc;
    if (speed < 0.4) {
        filter_desc = "atempo=0.5,atempo=0.5";
    } else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "atempo=%.6f", speed);
        filter_desc = tmp;
    }
    // Prefix with aresample so the input is always in fltp (required by atempo).
    std::string full_desc = "aresample=out_sample_fmt=fltp," + filter_desc;

    // Endpoint descriptors for graph parsing (follows FFmpeg filtering_audio.c example).
    AVFilterInOut* gout = avfilter_inout_alloc(); // source end
    AVFilterInOut* gin  = avfilter_inout_alloc(); // sink end
    if (!gout || !gin) {
        avfilter_inout_free(&gout);
        avfilter_inout_free(&gin);
        destroyFilterGraph();
        return false;
    }

    gout->name       = av_strdup("in");
    gout->filter_ctx = filter_src_;
    gout->pad_idx    = 0;
    gout->next       = nullptr;

    gin->name        = av_strdup("out");
    gin->filter_ctx  = filter_sink_;
    gin->pad_idx     = 0;
    gin->next        = nullptr;

    int ret = avfilter_graph_parse_ptr(filter_graph_, full_desc.c_str(),
                                       &gin, &gout, nullptr);
    // avfilter_graph_parse_ptr takes ownership; free whatever remains.
    avfilter_inout_free(&gout);
    avfilter_inout_free(&gin);

    if (ret < 0) {
        fprintf(stderr, "[AudioDecoder] avfilter_graph_parse_ptr failed: %d\n", ret);
        destroyFilterGraph();
        return false;
    }

    if (avfilter_graph_config(filter_graph_, nullptr) < 0) {
        fprintf(stderr, "[AudioDecoder] avfilter_graph_config failed\n");
        destroyFilterGraph();
        return false;
    }

    return true;
}

int AudioDecoder::resampleToS16(AVFrame* frame) {
    if (!codec_ctx_) return -1;

    if (!swr_ctx_) {
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout in_layout;
        av_channel_layout_copy(&in_layout, &frame->ch_layout);

        int ret = swr_alloc_set_opts2(&swr_ctx_,
            &out_layout, AV_SAMPLE_FMT_S16, Config::AUDIO_SAMPLE_RATE,
            &in_layout,  (AVSampleFormat)frame->format, frame->sample_rate,
            0, nullptr);
        av_channel_layout_uninit(&in_layout);

        if (ret < 0 || swr_init(swr_ctx_) < 0) {
            fprintf(stderr, "[AudioDecoder] SwrContext init failed\n");
            swr_free(&swr_ctx_);
            return -1;
        }
    }

    int max_out = (int)av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        Config::AUDIO_SAMPLE_RATE, frame->sample_rate, AV_ROUND_UP);

    int byte_count = max_out * Config::AUDIO_CHANNELS * 2; // 2 bytes per S16 sample
    if ((int)buf_.size() < byte_count) buf_.resize(byte_count);

    uint8_t* out_data[1] = { buf_.data() };
    int nb_out = swr_convert(swr_ctx_,
                             out_data, max_out,
                             (const uint8_t**)frame->data, frame->nb_samples);
    if (nb_out < 0) {
        fprintf(stderr, "[AudioDecoder] swr_convert failed\n");
        return -1;
    }

    return nb_out * Config::AUDIO_CHANNELS * 2;
}

void AudioDecoder::processFrame(AVFrame* frame, AudioOutput& output) {
    double speed = speed_.load();

    double pts_sec = 0.0;
    if (frame->pts != AV_NOPTS_VALUE)
        pts_sec = frame->pts * av_q2d(time_base_);
    else if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        pts_sec = frame->best_effort_timestamp * av_q2d(time_base_);

    if (std::fabs(speed - 1.0) < 1e-4) {
        // 1x speed: resample directly to S16.
        int bytes = resampleToS16(frame);
        if (bytes > 0) {
            double duration = (double)frame->nb_samples / frame->sample_rate;
            output.pushData(buf_.data(), bytes, pts_sec, duration);
        }
        av_frame_free(&frame);
        return;
    }

    // Non-1x: route through atempo filter graph.
    if (!filter_graph_) {
        if (!buildFilterGraph(frame)) {
            // Fallback: resample without speed change.
            int bytes = resampleToS16(frame);
            if (bytes > 0) {
                double duration = (double)frame->nb_samples / frame->sample_rate;
                output.pushData(buf_.data(), bytes, pts_sec, duration);
            }
            av_frame_free(&frame);
            return;
        }
    }

    if (av_buffersrc_add_frame_flags(filter_src_, frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
        av_frame_free(&frame);
        return;
    }
    av_frame_free(&frame);

    AVFrame* filt_frame = av_frame_alloc();
    while (true) {
        // Abort draining if the main thread requested a speed change.
        // This prevents old-speed audio from being pushed to SDL AFTER
        // Player::setSpeed() calls audio_->flush(), which would produce
        // a sustained crackling / electric-buzz artifact.
        if (speed_changed_.load()) break;

        int ret = av_buffersink_get_frame(filter_sink_, filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        int bytes = resampleToS16(filt_frame);
        if (bytes > 0) {
            // Each output sample now covers 'speed' times more media time.
            double duration = ((double)filt_frame->nb_samples / filt_frame->sample_rate) * speed;
            output.pushData(buf_.data(), bytes, pts_sec, duration);
            pts_sec += duration;
        }
        av_frame_unref(filt_frame);
    }
    av_frame_free(&filt_frame);
}

void AudioDecoder::decodeLoop(PacketQueue& pktQueue, AudioOutput& output) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) { running_.store(false); return; }

    while (running_.load()) {
        // Handle flush from seek.
        if (flush_requested_.load()) {
            flush_requested_.store(false);
            avcodec_flush_buffers(codec_ctx_);
            if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
            destroyFilterGraph();
            speed_changed_.store(false);
        }

        // Rebuild filter if speed was changed from outside.
        if (speed_changed_.load()) {
            speed_changed_.store(false);
            if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
            destroyFilterGraph();
        }

        AVPacket* pkt = pktQueue.pop();
        if (!pkt) break; // Queue aborted.

        // Re-check flush that was requested while pop() was blocked
        // (e.g. a seek arrived while we were waiting for the next packet).
        // This ensures avcodec_flush_buffers runs before the first post-seek
        // packet is decoded, preventing stale audio frames from corrupting
        // the audio clock.
        if (flush_requested_.load()) {
            flush_requested_.store(false);
            avcodec_flush_buffers(codec_ctx_);
            if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
            destroyFilterGraph();
            speed_changed_.store(false);
        }

        bool is_flush_pkt = (pkt->data == nullptr && pkt->size == 0);
        int ret = avcodec_send_packet(codec_ctx_, is_flush_pkt ? nullptr : pkt);
        av_packet_free(&pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            fprintf(stderr, "[AudioDecoder] avcodec_send_packet: %d\n", ret);
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                fprintf(stderr, "[AudioDecoder] avcodec_receive_frame: %d\n", ret);
                break;
            }

            AVFrame* out = av_frame_clone(frame);
            av_frame_unref(frame);
            if (out) processFrame(out, output);
        }
    }

    av_frame_free(&frame);
    running_.store(false);
}
