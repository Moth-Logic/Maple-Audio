#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// Unbounded thread-safe FIFO.
//
// Push never blocks: the capture loop must never wait on a consumer.
// Pop blocks until an item is available.
//
// Close() is used for shutdown. Items already queued are still delivered;
// Pop returns false only when the queue is both closed and empty. That is what
// keeps the final chunk from being dropped when a session stops.
template <typename T>
class ThreadSafeQueue {
public:
    // Returns false if the queue is closed; the item is not enqueued.
    bool Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            items_.push_back(std::move(value));
        }
        notEmpty_.notify_one();
        return true;
    }

    // Blocks until an item is available or the queue is closed and drained.
    bool Pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;

        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    // Non-blocking variant. Returns false if the queue is empty, even when open.
    bool TryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return false;

        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    // Wakes every blocked consumer. Safe to call more than once.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::deque<T> items_;
    bool closed_ = false;
};
