// --- START OF FILE file_manager.cpp ---

#include "file_manager.h"
#include "file_utils.h"
#include "app_config.h"
#include <iostream>
#include <cstring> // for strrchr

FileManager::FileManager() {}

FileManager::~FileManager() {
    // 确保在析构时线程一定被停止
    stop();
}

void FileManager::start() {
    if (!m_worker_thread.joinable()) {
        m_stop_flag = false;
        m_worker_thread = std::thread(&FileManager::worker_thread_func, this);
        std::cout << "[文件管理器] 后台线程已启动。" << std::endl;
    }
}

void FileManager::stop() {
    if (m_stop_flag.exchange(true)) {
        // 如果已经发送过停止信号，则直接返回
        return;
    }

    m_cv.notify_one(); // 唤醒可能正在等待的线程
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
    std::cout << "[文件管理器] 后台线程已停止。" << std::endl;
}

void FileManager::scheduleMove(const std::string& source_path) {
    if (source_path.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_task_queue.push(source_path);
    }
    m_cv.notify_one(); // 通知工作线程有新任务
}

void FileManager::worker_thread_func() {
    while (!m_stop_flag) {
        std::string src_path;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            // 等待条件变量，直到队列中有任务或收到退出信号
            m_cv.wait(lock, [this] {
                return !m_task_queue.empty() || m_stop_flag;
            });

            if (m_stop_flag && m_task_queue.empty()) {
                // 收到退出信号且所有任务已处理完毕
                break;
            }

            if (!m_task_queue.empty()) {
                src_path = m_task_queue.front();
                m_task_queue.pop();
            }
        }

        if (!src_path.empty()) {
            const char* slash = strrchr(src_path.c_str(), '/');
            const char* fname = slash ? slash + 1 : src_path.c_str();
            std::string dst_path = FINAL_STORAGE_PATH + std::string(fname);
            
            printf("[文件管理器] 正在移动 %s -> %s\n", src_path.c_str(), dst_path.c_str());
            
            if (move_file_robust(src_path.c_str(), dst_path.c_str()) != 0) {
                 fprintf(stderr, "[文件管理器] 错误: 文件移动失败: %s\n", src_path.c_str());
            }
        }
    }
    printf("[文件管理器] 工作线程正在退出循环。\n");
}