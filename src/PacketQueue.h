#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

// Thread-safe queue for AVPacket pointers.
// Supports an "abort" flag so blocking callers can wake up and exit.
class PacketQueue {
public:
    explicit PacketQueue(int maxSize = 256)
        : max_size_(maxSize), aborted_(false)
    {}

    ~PacketQueue() { flush(); }

    // Push a packet onto the queue. Takes ownership.
    // Blocks when full unless abort() has been called.
    // Returns false if aborted before the packet could be pushed.
    bool push(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_full_.wait(lock, [this] {
            return (int)queue_.size() < max_size_ || aborted_;
        });
        if (aborted_) {
            av_packet_free(&pkt);
            return false;
        }
        queue_.push_back(pkt);
        cv_not_empty_.notify_one();
        return true;
    }

    // Pop a packet from the queue. Caller takes ownership.
    // Blocks when empty unless abort() has been called.
    // Returns nullptr if aborted and queue is empty.
    AVPacket* pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [this] {
            return !queue_.empty() || aborted_;
        });
        if (queue_.empty()) return nullptr;
        AVPacket* pkt = queue_.front();
        queue_.pop_front();
        cv_not_full_.notify_one();
        return pkt;
    }

    // Try to pop without blocking. Returns nullptr if empty.
    AVPacket* tryPop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) return nullptr;
        AVPacket* pkt = queue_.front();
        queue_.pop_front();
        cv_not_full_.notify_one();
        return pkt;
    }

    // Flush all queued packets and free their memory.
    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (AVPacket* p : queue_) av_packet_free(&p);
        queue_.clear();
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    // Signal all waiting threads to unblock and return.
    void abort() {
        aborted_.store(true);
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    // Reset abort flag (call after flush before reuse).
    void resetAbort() {
        aborted_.store(false);
    }

    int size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (int)queue_.size();
    }

    bool isAborted() const { return aborted_.load(); }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<AVPacket*>   queue_;
    int                     max_size_;
    std::atomic<bool>       aborted_;
};
