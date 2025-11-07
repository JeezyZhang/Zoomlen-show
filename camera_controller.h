// --- START OF FILE camera_controller.h ---

#ifndef CAMERA_CONTROLLER_H
#define CAMERA_CONTROLLER_H

#include "camera_capture.h"
#include "recorder.h"
#include "osd_manager.h"
#include "zoom_manager.h"
#include "rtsp_streamer.h"
#include "exposure_manager.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>

// [重构] 向前声明 FileManager，避免在头文件中包含其完整定义
class FileManager;

/**
 * @class CameraController
 * @brief 内部核心控制器类，是整个 SDK 功能的“大脑”。
 */
class CameraController {
public:
    CameraController(std::string device_path);
    ~CameraController();

    bool initialize();

    int start_recording(const std::string& resolution);
    int stop_recording();
    int take_snapshot();
    void set_osd_enabled(bool enabled);
    void zoom_in();
    void zoom_out();
    int start_rtsp_stream(const std::string& url);
    int stop_rtsp_stream();
    void set_iso(int iso);
    void set_ev(double ev);

    std::shared_ptr<OsdManager> get_osd_manager();

private:
    std::string m_device_path;

    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    std::unique_ptr<ExposureManager> m_exposure_manager;
    
    // [重构] 新增 FileManager 成员，用于管理文件移动
    std::unique_ptr<FileManager> m_file_manager;

    std::unique_ptr<CameraCapture> m_camera_capture;

    std::unique_ptr<Recorder> m_recorder;
    std::thread m_recorder_thread;

    std::unique_ptr<RtspStreamer> m_streamer;
    std::thread m_streamer_thread;

    std::atomic<bool> m_is_recording{false};
    std::atomic<bool> m_is_streaming{false};
};

#endif // CAMERA_CONTROLLER_H