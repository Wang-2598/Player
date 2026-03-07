#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavutil/frame.h>
}

// Thread-safe queue for AVFrame pointers (used for decoded video frames).
// Supports peek (look without consuming) for AV-sync timing decisions.
class FrameQueue {
public:
    explicit FrameQueue(int maxSize = 8)
        : max_size_(maxSize), aborted_(false)
    {}

    ~FrameQueue() { flush(); }

    // Push a frame. Takes ownership. Blocks when full.
    // Returns false if aborted.
    bool push(AVFrame* frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_full_.wait(lock, [this] {
            return (int)queue_.size() < max_size_ || aborted_;
        });
        if (aborted_) {
            av_frame_free(&frame);
            return false;
        }
        queue_.push_back(frame);
        cv_not_empty_.notify_one();
        return true;
    }

    // Pop a frame. Caller takes ownership. Blocks when empty.
    // Returns nullptr if aborted and queue is empty.
    AVFrame* pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [this] {
            return !queue_.empty() || aborted_;
        });
        if (queue_.empty()) return nullptr;
        AVFrame* f = queue_.front();
        queue_.pop_front();
        cv_not_full_.notify_one();
        return f;
    }

    // Peek at the front frame without removing it.
    // Returns nullptr if queue is empty (non-blocking).
    AVFrame* peek() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return nullptr;
        return queue_.front();
    }

    // Flush all queued frames.
    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (AVFrame* f : queue_) av_frame_free(&f);
        queue_.clear();
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    // Signal waiting threads to unblock and return.
    void abort() {
        aborted_.store(true);
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    void resetAbort() {
        aborted_.store(false);
    }

    int size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (int)queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    bool isAborted() const { return aborted_.load(); }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<AVFrame*>    queue_;
    int                     max_size_;
    std::atomic<bool>       aborted_;
};
