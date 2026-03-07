#pragma once

#include "PacketQueue.h"
#include "AudioOutput.h"
#include "Config.h"
#include <thread>
#include <atomic>
#include <vector>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

// AudioDecoder reads packets from a PacketQueue, decodes them, resamples
// the audio to S16 stereo 44100 Hz (applying the atempo filter for speed
// control), and pushes the resulting PCM data to AudioOutput.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Initialise the audio codec from the given stream.
    bool init(const AVStream* stream);

    // Start the background audio thread.
    void start(PacketQueue& pktQueue, AudioOutput& output);

    // Stop the thread and release resources.
    void stop();

    // Flush decoder/filter state and any queued data (call on seek).
    void flush();

    // Change the playback speed. The audio thread rebuilds atempo on next frame.
    void setSpeed(double speed);
    double getSpeed() const { return speed_.load(); }

    // Codec name for OSD.
    const char* codecName() const;

private:
    void decodeLoop(PacketQueue& pktQueue, AudioOutput& output);

    // Send a raw decoded frame through the resampler (and optionally the
    // atempo filter) then push the resulting bytes to AudioOutput.
    void processFrame(AVFrame* frame, AudioOutput& output);

    // Build / rebuild the atempo filter graph for the current speed.
    bool buildFilterGraph(const AVFrame* ref_frame);
    void destroyFilterGraph();

    // Resample src_frame to S16 stereo 44100 Hz into buf_.
    // Returns number of bytes written, or -1 on error.
    int resampleToS16(AVFrame* src_frame);

    AVCodecContext* codec_ctx_   = nullptr;
    SwrContext*     swr_ctx_     = nullptr;

    // atempo filter graph
    AVFilterGraph*   filter_graph_  = nullptr;
    AVFilterContext* filter_src_    = nullptr;
    AVFilterContext* filter_sink_   = nullptr;

    AVRational      time_base_   = {0, 1};
    std::atomic<double> speed_   {1.0};

    // Set by setSpeed() (main thread); checked in audio thread to trigger rebuild.
    std::atomic<bool>  speed_changed_ {false};

    // Scratch buffer for resampled S16 output.
    std::vector<uint8_t> buf_;

    std::thread       thread_;
    std::atomic<bool> running_          {false};
    std::atomic<bool> flush_requested_  {false};
};
