#include "VideoDecoder.h"
#include "Config.h"
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

// Callback used during codec open to select the correct HW pixel format.
static AVPixelFormat s_hw_pix_fmt = AV_PIX_FMT_NONE;

static AVPixelFormat getHWFormat(AVCodecContext* /*ctx*/, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == s_hw_pix_fmt) return *p;
    }
    fprintf(stderr, "[VideoDecoder] HW pixel format not available, using SW fallback\n");
    return formats[0];
}

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    stop();
}

bool VideoDecoder::init(const AVStream* stream, DecodeMode mode) {
    if (!stream) return false;
    time_base_ = stream->time_base;

    const AVCodec* codec = nullptr;

    // Try hardware decoding first.
    if (mode == DecodeMode::Hardware) {
        // Map codec_id to cuvid (NVDEC) decoder name.
        const char* hwCodecName = nullptr;
        switch (stream->codecpar->codec_id) {
            case AV_CODEC_ID_H264:       hwCodecName = "h264_cuvid";   break;
            case AV_CODEC_ID_HEVC:       hwCodecName = "hevc_cuvid";   break;
            case AV_CODEC_ID_VP9:        hwCodecName = "vp9_cuvid";    break;
            case AV_CODEC_ID_VP8:        hwCodecName = "vp8_cuvid";    break;
            case AV_CODEC_ID_AV1:        hwCodecName = "av1_cuvid";    break;
            case AV_CODEC_ID_MPEG2VIDEO: hwCodecName = "mpeg2_cuvid";  break;
            case AV_CODEC_ID_MPEG4:      hwCodecName = "mpeg4_cuvid";  break;
            case AV_CODEC_ID_VC1:        hwCodecName = "vc1_cuvid";    break;
            default: break;
        }
        if (hwCodecName) {
            codec = avcodec_find_decoder_by_name(hwCodecName);
            if (codec) {
                is_hardware_ = true;
            } else {
                fprintf(stderr, "[VideoDecoder] HW codec '%s' not found, falling back to SW\n", hwCodecName);
            }
        }

        // If cuvid codec not found, try hwdevice approach with CUDA.
        if (!codec) {
            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                // Try to create a CUDA hardware device context.
                AVBufferRef* hw_ctx = nullptr;
                if (av_hwdevice_ctx_create(&hw_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
                    // Find the hw pixel format supported by CUDA for this codec.
                    for (int i = 0;; ++i) {
                        const AVCodecHWConfig* hwcfg = avcodec_get_hw_config(codec, i);
                        if (!hwcfg) break;
                        if ((hwcfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                            hwcfg->device_type == AV_HWDEVICE_TYPE_CUDA) {
                            s_hw_pix_fmt  = hwcfg->pix_fmt;
                            hw_pix_fmt_   = hwcfg->pix_fmt;
                            hw_device_ctx_ = hw_ctx;
                            break;
                        }
                    }
                    if (!hw_device_ctx_)
                        av_buffer_unref(&hw_ctx);
                    else
                        is_hardware_ = true;
                } else {
                    fprintf(stderr, "[VideoDecoder] CUDA device creation failed, using SW\n");
                }
            }
        }
    }

    // Fall back to software decoder.
    if (!codec)
        codec = avcodec_find_decoder(stream->codecpar->codec_id);

    if (!codec) {
        fprintf(stderr, "[VideoDecoder] No decoder found for codec id %d\n", stream->codecpar->codec_id);
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        fprintf(stderr, "[VideoDecoder] Failed to copy codec parameters\n");
        return false;
    }

    // Attach hardware device context.
    if (hw_device_ctx_) {
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        codec_ctx_->get_format    = getHWFormat;
    }

    codec_ctx_->thread_count = 0; // Let FFmpeg choose optimal thread count.

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "refcounted_frames", "1", 0);

    int ret = avcodec_open2(codec_ctx_, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        fprintf(stderr, "[VideoDecoder] avcodec_open2 failed\n");
        avcodec_free_context(&codec_ctx_);
        if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
        return false;
    }

    fprintf(stdout, "[VideoDecoder] Using codec: %s (%s)\n",
            codec->long_name, is_hardware_ ? "Hardware" : "Software");
    return true;
}

void VideoDecoder::start(PacketQueue& pktQueue) {
    if (running_.load()) return;
    running_.store(true);
    frame_queue_.resetAbort();
    thread_ = std::thread(&VideoDecoder::decodeLoop, this, std::ref(pktQueue));
}

void VideoDecoder::stop() {
    running_.store(false);
    frame_queue_.abort();
    if (thread_.joinable()) thread_.join();

    if (codec_ctx_)    { avcodec_free_context(&codec_ctx_); }
    if (hw_device_ctx_){ av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
}

void VideoDecoder::flush() {
    flush_pending_.store(true);
    frame_queue_.flush();
}

void VideoDecoder::setSkipUntilPts(double pts) {
    skip_until_pts_.store(pts);
}

const char* VideoDecoder::codecName() const {
    if (codec_ctx_ && codec_ctx_->codec)
        return codec_ctx_->codec->name;
    return "unknown";
}

double VideoDecoder::fps() const {
    if (!codec_ctx_) return 0.0;
    AVRational r = codec_ctx_->framerate;
    if (r.num == 0) return 0.0;
    return static_cast<double>(r.num) / r.den;
}

AVFrame* VideoDecoder::transferHWFrame(AVFrame* hw_frame) {
    AVFrame* sw_frame = av_frame_alloc();
    if (!sw_frame) return nullptr;

    int ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    if (ret < 0) {
        fprintf(stderr, "[VideoDecoder] HW frame transfer failed\n");
        av_frame_free(&sw_frame);
        return nullptr;
    }
    // Copy PTS and other metadata from the hardware frame.
    sw_frame->pts             = hw_frame->pts;
    sw_frame->pkt_dts         = hw_frame->pkt_dts;
    sw_frame->best_effort_timestamp = hw_frame->best_effort_timestamp;
    av_frame_free(&hw_frame);
    return sw_frame;
}

void VideoDecoder::decodeLoop(PacketQueue& pktQueue) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) { running_.store(false); return; }

    while (running_.load()) {
        // Handle pending flush (seek): must be done on this thread.
        if (flush_pending_.load()) {
            avcodec_flush_buffers(codec_ctx_);
            flush_pending_.store(false);
        }

        AVPacket* pkt = pktQueue.pop();
        if (!pkt) break; // Aborted.

        // Re-check flush that was requested while pop() was blocked.
        if (flush_pending_.load()) {
            avcodec_flush_buffers(codec_ctx_);
            flush_pending_.store(false);
        }

        bool is_flush = (pkt->data == nullptr && pkt->size == 0);

        int ret = avcodec_send_packet(codec_ctx_, is_flush ? nullptr : pkt);
        av_packet_free(&pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            if (ret != AVERROR_EOF)
                fprintf(stderr, "[VideoDecoder] avcodec_send_packet: %d\n", ret);
            // On EOF, drain remaining frames below.
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                fprintf(stderr, "[VideoDecoder] avcodec_receive_frame: %d\n", ret);
                break;
            }

            // Transfer from GPU to CPU if necessary.
            AVFrame* out_frame = frame;
            if (frame->hw_frames_ctx || (hw_device_ctx_ && frame->format == hw_pix_fmt_)) {
                AVFrame* tmp = av_frame_clone(frame);
                av_frame_unref(frame);
                out_frame = transferHWFrame(tmp);
                if (!out_frame) continue;
            } else {
                out_frame = av_frame_clone(frame);
                av_frame_unref(frame);
            }

            // Fast-forward through keyframe-to-target gap: discard frames
            // before the seek target entirely, without touching frame_queue_.
            // This lets the decoder run at full speed without any vsync stalls.
            double skip_pts = skip_until_pts_.load();
            if (skip_pts >= 0.0) {
                int64_t ts = (out_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                ? out_frame->best_effort_timestamp : out_frame->pts;
                double fpts = (ts != AV_NOPTS_VALUE)
                                ? ts * av_q2d(time_base_) : skip_pts;
                if (fpts < skip_pts) {
                    av_frame_free(&out_frame);
                    continue;
                }
                skip_until_pts_.store(-1.0); // reached target — disable
            }

            if (!frame_queue_.push(out_frame)) {
                // Queue aborted; push() already freed the frame.
                break;
            }
        }
    }

    av_frame_free(&frame);
    running_.store(false);
}