#include "camera_controller.h"
#include "snapshotter.h"
#include "app_config.h"
#include "file_utils.h"

#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>

extern "C"
{
#include <libavdevice/avdevice.h>  // 包含FFmpeg设备库，用于设备输入输出
#include <libavutil/log.h>         // 包含FFmpeg日志库，用于日志输出
}

/**
 * @file camera_controller.cpp
 * @brief 实现了 CameraController 类的功能，作为 SDK 的核心逻辑。
 */

// 使用匿名命名空间来隐藏这些全局变量和函数，使其只在本文件内可见。
// 这是 C++ 中替代 'static' 全局变量的现代做法。
namespace
{
    // 全局的、线程安全的文件移动任务队列
    std::queue<std::string> g_file_move_queue;
    std::mutex g_queue_mutex;
    std::condition_variable g_cv;
    std::atomic<bool> g_exit_file_manager(false);
    std::thread g_file_manager_thread;

    /**
     * @brief 媒体文件（视频/照片）处理完成后的回调函数。
     * * 当 Recorder 或 Snapshotter 在 /tmp 目录成功生成文件后，会调用此函数。
     * 此函数将文件路径放入一个全局队列，并通知文件管理器线程去处理它。
     * @param temp_filepath 生成的临时文件的完整路径。
     */
    void on_media_finished(const std::string &temp_filepath)
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_file_move_queue.push(temp_filepath);
        g_cv.notify_one(); // 唤醒可能正在等待的文件管理器线程
    }

    /**
     * @brief 文件管理器线程的主函数。
     * * 这是一个常驻后台线程，负责从队列中取出文件路径，
     * 并将文件从临时目录（如 /tmp）移动到最终存储目录（如 /mnt/sdcard）。
     */
    void file_manager_thread_func()
    {
        while (!g_exit_file_manager)
        {
            std::string src_path;
            {
                std::unique_lock<std::mutex> lock(g_queue_mutex);
                // 等待条件变量，直到队列中有任务或收到退出信号
                g_cv.wait(lock, []
                          { return !g_file_move_queue.empty() || g_exit_file_manager; });

                if (g_exit_file_manager && g_file_move_queue.empty())
                    break; // 收到退出信号且任务已完成

                if (!g_file_move_queue.empty())
                {
                    src_path = g_file_move_queue.front();
                    g_file_move_queue.pop();
                }
            }
            if (!src_path.empty())
            {
                const char *slash = strrchr(src_path.c_str(), '/');
                const char *fname = slash ? slash + 1 : src_path.c_str();
                std::string dst_path = FINAL_STORAGE_PATH + std::string(fname);
                printf("[文件管理器] 正在移动 %s -> %s\n", src_path.c_str(), dst_path.c_str());
                move_file_robust(src_path.c_str(), dst_path.c_str());
            }
        }
        printf("[文件管理器] 线程正在退出。\n");
    }
} // 匿名命名空间结束

CameraController::CameraController(std::string device_path)
    : m_device_path(std::move(device_path)) {}

CameraController::~CameraController()
{
    // 确保在控制器销毁时，所有后台线程都能被安全地停止和清理。

    // 如果仍在录制，则先停止它
    if (m_is_recording)
    {
        stop_recording();
    }

    // 确保任何可能存在的、已结束但未被 join 的线程被 join
    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }

    // 向文件管理器线程发送退出信号并唤醒它
    g_exit_file_manager = true;
    g_cv.notify_one();
    if (g_file_manager_thread.joinable())
    {
        g_file_manager_thread.join();
    }

    // 关闭 OSD 管理器（它也有自己的内部线程）
    if (m_osd_manager)
    {
        m_osd_manager->shutdown();
    }
}

bool CameraController::initialize()
{
    // 初始化所有子模块
    m_osd_manager = std::make_shared<OsdManager>();
    if (!m_osd_manager->initialize())
    {
        std::cerr << "错误: OSD管理器初始化失败。" << std::endl;
        return false;
    }
    m_zoom_manager = std::make_shared<ZoomManager>();

    // 启动文件管理器后台线程
    g_file_manager_thread = std::thread(file_manager_thread_func);

    // 注册所有 FFmpeg 设备，这是使用 V4L2 前的必需步骤
    avdevice_register_all();

    return true;
}

int CameraController::start_recording(const std::string &resolution)
{
    if (m_is_recording)
    {
        std::cerr << "错误: 录制已在进行中。" << std::endl;
        return -1;
    }

    // 确保上一个录制线程（如果存在）已经结束
    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }

    // 创建一个新的 Recorder 实例进行本次录制
    m_recorder.reset(new Recorder(m_device_path, m_osd_manager, m_zoom_manager, on_media_finished));

    if (!m_recorder->prepare(resolution))
    {
        m_recorder.reset(); // 准备失败，清理实例
        return -1;
    }

    m_is_recording = true;
    // 启动新的录制线程
    m_recorder_thread = std::thread([this]()
                                    { 
        if (m_recorder) m_recorder->run();
        // 录制结束后，在录制线程内部将状态标志设为 false
        m_is_recording = false; });
    return 0;
}

int CameraController::stop_recording()
{
    if (!m_is_recording)
    {
        // 即使状态标志为 false，也检查一下线程是否还在运行（例如，启动后立即出错）
        if (m_recorder_thread.joinable())
        {
            if (m_recorder)
                m_recorder->stop();
            m_recorder_thread.join();
            m_recorder.reset();
            std::cout << "录制已停止。" << std::endl;
            return 0;
        }
        std::cerr << "错误: 当前没有在录制。" << std::endl;
        return -1;
    }

    // 向录制线程发送停止信号
    if (m_recorder)
    {
        m_recorder->stop();
    }
    // 等待录制线程结束
    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }
    m_recorder.reset();     // 销毁 Recorder 实例
    m_is_recording = false; // 确保状态被重置
    std::cout << "录制已停止。" << std::endl;
    return 0;
}

int CameraController::take_snapshot()
{
    // Snapshotter 被设计为“一次性”对象，它会在后台线程任务完成后自我删除。
    std::cout << "进入take_snapshot函数" << std::endl;
    auto *snapshotter = new Snapshotter(m_device_path, m_osd_manager, m_zoom_manager, on_media_finished);
    snapshotter->shoot();
    return 0;
}

void CameraController::set_osd_enabled(bool enabled)
{
    if (m_osd_manager)
    {
        m_osd_manager->enable(enabled);
    }
}

void CameraController::zoom_in()
{
    if (m_zoom_manager)
    {
        m_zoom_manager->zoom_in();
    }
}

void CameraController::zoom_out()
{
    if (m_zoom_manager)
    {
        m_zoom_manager->zoom_out();
    }
}
