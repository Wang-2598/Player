#pragma once

#include "PacketQueue.h"
#include "Config.h"
#include <string>
#include <thread>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
}

// Demuxer opens a media file, finds audio/video streams, and continuously reads
// packets, routing them to the appropriate PacketQueue.
// Seeking is requested from outside and executed inside the demux thread.
class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    // Open a media file. Returns false on error.
    bool open(const std::string& filename);

    // Start the background demux thread.
    void start();

    // Stop demuxing and close the file.
    void stop();

    // Request a seek to the given media time in seconds.
    // The actual seek is performed by the demux thread.
    void requestSeek(double seconds);

    // Flush all queued packets (call before/after seek from outside).
    void flushQueues();

    // Access the packet queues (used by decoders).
    PacketQueue& videoQueue() { return video_queue_; }
    PacketQueue& audioQueue() { return audio_queue_; }

    // Stream indices (-1 if not present).
    int videoStreamIndex() const { return video_stream_idx_; }
    int audioStreamIndex() const { return audio_stream_idx_; }

    // Format context accessors (read-only, valid after open()).
    const AVStream* videoStream() const;
    const AVStream* audioStream() const;

    double duration() const { return duration_; }
    bool   isEOF()    const { return eof_.load(); }

private:
    void demuxLoop();

    AVFormatContext* fmt_ctx_         = nullptr;
    int              video_stream_idx_ = -1;
    int              audio_stream_idx_ = -1;
    double           duration_         = 0.0;

    PacketQueue video_queue_;
    PacketQueue audio_queue_;

    std::thread      thread_;
    std::atomic<bool> running_  {false};
    std::atomic<bool> eof_      {false};

    // Seek request
    std::atomic<bool>   seek_requested_ {false};
    std::atomic<double> seek_target_    {0.0};
};
