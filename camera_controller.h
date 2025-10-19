#ifndef CAMERA_CONTROLLER_H
#define CAMERA_CONTROLLER_H

#include "recorder.h"
#include "osd_manager.h"
#include "zoom_manager.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>

/**
 * @class CameraController
 * @brief 内部核心控制器类，是整个 SDK 功能的“大脑”。
 *
 * 这个类对最终用户（甲方）是隐藏的。它负责：
 * - 创建和管理所有功能模块（OSD, Zoom, Recorder, 文件管理器等）的生命周期。
 * - 维护系统的核心状态（例如，是否正在录制）。
 * - 作为 C 语言 API 和内部 C++ 模块之间的桥梁。
 */
class CameraController {
public:
    // 构造函数，接收设备路径
    CameraController(std::string device_path);
    // 析构函数，负责资源的最终清理
    ~CameraController();

    // 初始化所有子模块和后台线程
    bool initialize();

    // 公开给 C API 调用的功能接口
    int start_recording(const std::string& resolution);
    int stop_recording();
    int take_snapshot();
    void set_osd_enabled(bool enabled);
    void zoom_in();
    void zoom_out();

private:
    std::string m_device_path;

    // 使用智能指针管理各个模块的生命周期
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    
    // 使用 unique_ptr 管理 Recorder，因为它与一个特定的录制线程紧密绑定。
    // 每次录制都是一个新的 Recorder 实例和一个新的线程。
    std::unique_ptr<Recorder> m_recorder;
    std::thread m_recorder_thread;

    // 内部状态标志，用于防止重复操作
    std::atomic<bool> m_is_recording{false};
};

#endif // CAMERA_CONTROLLER_H

