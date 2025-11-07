// --- START OF FILE camera_controller.cpp ---

#include "camera_controller.h"
#include "snapshotter.h"
#include "app_config.h"
#include "file_manager.h"
#include "camera_capture.h"

#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <thread>   // [新增]
#include <chrono>   // [新增]

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavutil/log.h>
#include <libavformat/avformat.h>
}

std::mutex g_camera_device_mutex;

CameraController::CameraController(std::string device_path)
    : m_device_path(std::move(device_path)) {}

CameraController::~CameraController()
{
    if (m_is_recording)
    {
        stop_recording();
    }
    if (m_is_streaming)
    {
        stop_rtsp_stream();
    }

    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }
    if (m_streamer_thread.joinable())
    {
        m_streamer_thread.join();
    }

    if (m_camera_capture)
    {
        m_camera_capture->stop();
    }

    if (m_file_manager)
    {
        m_file_manager->stop();
    }

    if (m_osd_manager)
    {
        m_osd_manager->shutdown();
    }

    if (m_exposure_manager)
    {
        m_exposure_manager->stop();
    }
}

bool CameraController::initialize()
{
    m_osd_manager = std::make_shared<OsdManager>();
    if (!m_osd_manager->initialize())
    {
        std::cerr << "错误: OSD管理器初始化失败。" << std::endl;
        return false;
    }
    m_zoom_manager = std::make_shared<ZoomManager>();

    std::string subdev_path = "/dev/v4l-subdev2";
    m_exposure_manager = std::make_unique<ExposureManager>(subdev_path);
    m_exposure_manager->start();

    m_file_manager = std::make_unique<FileManager>();
    m_file_manager->start();

    avdevice_register_all();
    avformat_network_init();

    m_camera_capture = std::make_unique<CameraCapture>(m_device_path);
    if (!m_camera_capture->start())
    {
        std::cerr << "错误: 核心摄像头采集模块启动失败。" << std::endl;
        m_camera_capture.reset();
        return false;
    }

    m_zoom_manager->check_and_reset_change_flag();

    std::cout << "[CameraController] 初始化成功, 核心采集已启动。" << std::endl;
    return true;
}

int CameraController::start_recording(const std::string &resolution)
{
    if (m_is_recording)
    {
        std::cerr << "错误: 录制已在进行中。" << std::endl;
        return -1;
    }

    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }

    m_zoom_manager->check_and_reset_change_flag();

    auto on_media_finished_callback = [this](const std::string& temp_filepath) {
        if (m_file_manager) {
            m_file_manager->scheduleMove(temp_filepath);
        }
    };

    m_recorder = std::make_unique<Recorder>(m_camera_capture.get(), m_osd_manager, m_zoom_manager, on_media_finished_callback);

    if (!m_recorder->prepare(resolution))
    {
        m_recorder.reset();
        return -1;
    }

    m_is_recording = true;
    m_recorder_thread = std::thread([this]()
                                    { 
        if (m_recorder) m_recorder->run();
        m_is_recording = false; });
    return 0;
}

int CameraController::stop_recording()
{
    if (!m_is_recording)
    {
        // 即使状态标志为 false，也检查一下线程是否还在运行
        if (m_recorder_thread.joinable())
        {
            if (m_recorder)
                m_recorder->stop();
            m_recorder_thread.join();
        }
        std::cerr << "错误: 当前没有在录制。" << std::endl;
        return -1;
    }

    if (m_recorder)
    {
        m_recorder->stop();
    }
    if (m_recorder_thread.joinable())
    {
        m_recorder_thread.join();
    }
    m_recorder.reset();
    m_is_recording = false;
    std::cout << "录制已停止。" << std::endl;
    
    // [核心修复] 在停止一个重量级硬件用户后，短暂等待，给驱动清理时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}

int CameraController::take_snapshot()
{
    std::cout << "进入take_snapshot函数" << std::endl;
    
    auto on_media_finished_callback = [this](const std::string& temp_filepath) {
        if (m_file_manager) {
            m_file_manager->scheduleMove(temp_filepath);
        }
    };

    auto snapshotter = std::make_shared<Snapshotter>(m_camera_capture.get(), m_osd_manager, m_zoom_manager, on_media_finished_callback);
    
    std::thread([snapshotter]() {
        snapshotter->run();
    }).detach();

    return 0;
}

void CameraController::set_osd_enabled(bool enabled)
{
    if (m_osd_manager)
    {
        m_osd_manager->enable(enabled);
    }
}

int CameraController::start_rtsp_stream(const std::string &url)
{
    if (m_is_streaming)
    {
        std::cerr << "错误: RTSP推流已在进行中。" << std::endl;
        return -1;
    }

    if (m_streamer_thread.joinable())
    {
        m_streamer_thread.join();
    }

    m_zoom_manager->check_and_reset_change_flag();

    m_streamer = std::make_unique<RtspStreamer>(m_camera_capture.get(), m_osd_manager, m_zoom_manager);

    if (!m_streamer->prepare(url))
    {
        m_streamer.reset();
        return -1;
    }

    m_is_streaming = true;
    m_streamer_thread = std::thread([this]()
                                    {
        if (m_streamer) m_streamer->run();
        m_is_streaming = false; });
    return 0;
}

int CameraController::stop_rtsp_stream()
{
    if (!m_is_streaming)
    {
        // 即使状态标志为 false，也检查一下线程是否还在运行
        if (m_streamer_thread.joinable())
        {
            if (m_streamer)
                m_streamer->stop();
            m_streamer_thread.join();
        }
        std::cerr << "错误: 当前没有在推流。" << std::endl;
        return -1;
    }

    if (m_streamer)
    {
        m_streamer->stop();
    }
    if (m_streamer_thread.joinable())
    {
        m_streamer_thread.join();
    }
    m_streamer.reset();
    m_is_streaming = false;
    std::cout << "RTSP推流已停止。" << std::endl;

    // [核心修复] 在停止一个重量级硬件用户后，短暂等待，给驱动清理时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
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

void CameraController::set_iso(int iso)
{
    if (m_exposure_manager)
    {
        m_exposure_manager->set_iso(iso);
    }
}

void CameraController::set_ev(double ev)
{
    if (m_exposure_manager)
    {
        m_exposure_manager->set_ev(ev);
    }
}

std::shared_ptr<OsdManager> CameraController::get_osd_manager()
{
    return m_osd_manager;
}