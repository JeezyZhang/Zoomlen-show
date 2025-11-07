// --- START OF FILE threadsafe_queue.h ---

#ifndef THREADSAFE_QUEUE_H
#define THREADSAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory> // [重构] 包含 <memory> 以使用智能指针

// [重构] 包含 AVFrame 定义
extern "C"
{
#include <libavutil/frame.h>
}

// [重构] 定义一个 AVFrame 的智能指针类型，并附带自定义删除器。
// 这样，当 shared_ptr 的引用计数归零时，它会自动调用 av_frame_free 来释放资源。
using AVFramePtr = std::shared_ptr<AVFrame>;

// [重构] 提供一个辅助函数，方便地从 AVFrame* 创建我们的智能指针。
inline AVFramePtr make_avframe_ptr(AVFrame* frame) {
    if (!frame) return nullptr;
    return AVFramePtr(frame, [](AVFrame* f){ av_frame_free(&f); });
}


/**
 * @class ThreadSafeFrameQueue
 * @brief 一个专门为 AVFramePtr (AVFrame的智能指针) 优化的线程安全阻塞队列。
 */
class ThreadSafeFrameQueue
{
public:
    ThreadSafeFrameQueue() : m_stop(false) {}

    /**
     * @brief 生产者调用：将一个帧推入队列。
     * @param frame_ptr 指向帧的智能指针。
     */
    void push(AVFramePtr frame_ptr)
    {
        if (!frame_ptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
        {
            return;
        }
        m_queue.push(std::move(frame_ptr)); // [优化] 使用 std::move 提升效率
        m_cv.notify_one();
    }

    /**
     * @brief 消费者调用：等待并弹出一个帧。
     * @return 包含帧的智能指针，或在队列停止时返回 nullptr。
     */
    AVFramePtr wait_and_pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]
                  { return !m_queue.empty() || m_stop; });

        if (m_stop && m_queue.empty())
        {
            return nullptr;
        }
        if (m_queue.empty())
        {
            return nullptr;
        }

        AVFramePtr frame_ptr = std::move(m_queue.front()); // [优化] 使用 std::move
        m_queue.pop();
        return frame_ptr;
    }

    /**
     * @brief 停止队列，唤醒所有等待中的线程。
     */
    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cv.notify_all();
    }

    /**
     * @brief 清空队列中的所有帧。
     */
    inline void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // [重构] 无需手动释放，智能指针会自动处理。只需清空队列即可。
        std::queue<AVFramePtr> empty_queue;
        m_queue.swap(empty_queue);
    }

private:
    std::queue<AVFramePtr> m_queue; // [重构] 存储 AVFramePtr 而不是 AVFrame*
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop;
};

#endif // THREADSAFE_QUEUE_H