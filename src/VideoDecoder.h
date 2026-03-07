#pragma once

#include "PacketQueue.h"
#include "FrameQueue.h"
#include "Config.h"
#include <string>
#include <thread>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

// Decoding mode: hardware-accelerated (NVDEC via CUDA) or software CPU.
enum class DecodeMode { Software, Hardware };

// VideoDecoder reads packets from a PacketQueue, decodes them using either
// a hardware (NVDEC/CUDA) or software codec, and places decoded AVFrames
// (always in CPU memory) into a FrameQueue.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool init(const AVStream* stream, DecodeMode mode);
    void start(PacketQueue& pktQueue);
    void stop();

    // Signal a flush (seek). avcodec_flush_buffers is called inside the
    // decode thread at the next safe point — no cross-thread codec access.
    void flush();

    // After a seek, instruct the decode thread to silently discard all decoded
    // frames with PTS < pts without queuing them.  Pass a negative value to
    // disable (reset to normal operation).
    void setSkipUntilPts(double pts);

    FrameQueue& frameQueue() { return frame_queue_; }

    const char*  codecName()  const;
    int          width()      const { return codec_ctx_ ? codec_ctx_->width  : 0; }
    int          height()     const { return codec_ctx_ ? codec_ctx_->height : 0; }
    AVRational   timeBase()   const { return time_base_; }
    double       fps()        const;
    bool         isHardware() const { return is_hardware_; }

private:
    void decodeLoop(PacketQueue& pktQueue);
    // Transfer a hardware (GPU) frame to system memory and return a new
    // AVFrame in a CPU pixel format. Frees hw_frame on success.
    AVFrame* transferHWFrame(AVFrame* hw_frame);

    AVCodecContext* codec_ctx_     = nullptr;
    AVBufferRef*    hw_device_ctx_ = nullptr;
    AVPixelFormat   hw_pix_fmt_    = AV_PIX_FMT_NONE;
    AVRational      time_base_     = {0, 1};
    bool            is_hardware_   = false;

    FrameQueue frame_queue_{Config::VIDEO_FRAME_QUEUE_SIZE};

    std::thread       thread_;
    std::atomic<bool> running_       {false};
    // Flush flag: set by flush() on main thread, cleared inside decode thread.
    std::atomic<bool> flush_pending_ {false};
    // Frames with PTS < skip_until_pts_ are discarded inside the decode thread
    // to fast-forward through the keyframe-to-seek-target gap without stalls.
    std::atomic<double> skip_until_pts_ {-1.0};
};
