#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <chrono>

namespace uei
{
    /**
     * @brief Thread-safe ring buffer with "Drop Oldest" overwrite policy.
     *
     * - Producer: push(item) never blocks.
     * - If buffer is full, it drops the oldest item and keeps the newest.
     * - Consumer: pop(item, timeout) blocks until data available or timeout.
     *
     * @tparam T element type
     */
    template <typename T>
    class RingBuffer
    {
    public:
        /**
         * @brief Construct a ring buffer.
         * @param capacity Maximum number of elements to keep.
         */
        explicit RingBuffer(std::size_t capacity)
            : capacity_(capacity)
        {
        }

        /**
         * @brief Push an item into buffer. Never blocks.
         * @param item Item to push (copied).
         */
        void Push(const T &item)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            PushLocked(item);
            cv_.notify_one();
        }

        /**
         * @brief Push an item into buffer. Never blocks.
         * @param item Item to push (moved).
         */
        void Push(T &&item)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            PushLocked(std::move(item));
            cv_.notify_one();
        }

        /**
         * @brief Pop an item with timeout.
         * @param out Output item.
         * @param timeout Maximum wait duration.
         * @return true if got an item, false if timeout or stopped.
         */
        bool PopFor(T &out, std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (!cv_.wait_for(lk, timeout, [&]()
                              { return stop_ || !q_.empty(); }))
                return false;

            if (stop_)
                return false;

            out = std::move(q_.front());
            q_.pop_front();
            return true;
        }

        /**
         * @brief Stop the buffer. Wakes all waiting consumers.
         */
        void Stop()
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
            cv_.notify_all();
        }

        /**
         * @brief Current size.
         */
        std::size_t Size() const
        {
            std::lock_guard<std::mutex> lk(mtx_);
            return q_.size();
        }

        /**
         * @brief Capacity.
         */
        std::size_t Capacity() const { return capacity_; }

    private:
        template <typename U>
        void PushLocked(U &&item)
        {
            if (capacity_ == 0)
                return;

            if (q_.size() >= capacity_)
            {
                // Drop oldest
                q_.pop_front();
            }
            q_.push_back(std::forward<U>(item));
        }

        const std::size_t capacity_;
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<T> q_;
        bool stop_{false};
    };

} // namespace uei
