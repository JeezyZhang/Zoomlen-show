// --- START OF FILE file_manager.h ---

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @class FileManager
 * @brief 负责在后台异步、安全地移动文件。
 *
 * 这个类将文件移动操作封装在一个独立的常驻线程中，
 * 通过一个线程安全的队列接收任务，避免阻塞核心业务逻辑。
 */
class FileManager {
public:
    FileManager();
    ~FileManager();

    // 启动后台工作线程
    void start();

    // 请求停止后台工作线程并等待其结束
    void stop();

    /**
     * @brief 向队列中添加一个文件移动任务。
     * @param source_path 要移动的源文件路径。
     */
    void scheduleMove(const std::string& source_path);

private:
    // 后台工作线程的主函数
    void worker_thread_func();

    std::thread m_worker_thread;
    std::queue<std::string> m_task_queue;
    std::mutex m_queue_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop_flag{false};
};

#endif // FILE_MANAGER_H