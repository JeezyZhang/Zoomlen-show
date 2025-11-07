#include "exposure_manager.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cstring> // For memset and strerror
#include <algorithm> // For std::min and std::max


// --- 工具函数，源自 demo.cpp ---

// [修复] 移除了未被使用的 mapRange 函数以消除警告
// static double mapRange(int value, int in_min, int in_max, double out_min, double out_max) {
//     return out_min + (double)(value - in_min) * (out_max - out_min) / (double)(in_max - in_min);
// }

static int mapRangeReverse(double value, double out_min, double out_max, int in_min, int in_max) {
    return (int)(in_min + (value - out_min) * (in_max - in_min) / (out_max - out_min));
}

// --- 类实现 ---

ExposureManager::ExposureManager(std::string device_path)
    : m_device_path(std::move(device_path)) {}

ExposureManager::~ExposureManager() {
    stop();
}

void ExposureManager::start() {
    m_stop_flag = false;
    m_thread = std::thread(&ExposureManager::run, this);
}

void ExposureManager::stop() {
    if (m_stop_flag.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_flag = true;
    }
    m_cv.notify_one();

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ExposureManager::set_iso(int iso) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_iso_target = iso;
    m_new_request = true;
    m_cv.notify_one();
}

void ExposureManager::set_ev(double ev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ev_target = ev;
    m_new_request = true;
    m_cv.notify_one();
}

void ExposureManager::run() {
    while (!m_stop_flag.load()) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_new_request || m_stop_flag.load(); });

        if (m_stop_flag.load()) break;

        if (m_new_request) {
            apply_settings();
            m_new_request = false; // 在锁内重置标志
        }
    }
}

void ExposureManager::apply_settings() {
    // 这个函数在已持有锁的情况下被run()调用
    int fd = open(m_device_path.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "[ExposureManager] 错误: 无法打开设备 " << m_device_path << ": " << strerror(errno) << std::endl;
        // 重置请求，避免下次循环再次尝试
        m_iso_target = -1;
        m_ev_target = 999;
        return;
    }

    struct v4l2_queryctrl qctrl;
    struct v4l2_control ctrl;
    int gain_min = 0, gain_max = 0;
    int exp_min = 0, exp_max = 0;

    // 查询增益(gain)控件
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = V4L2_CID_ANALOGUE_GAIN;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        gain_min = qctrl.minimum;
        gain_max = qctrl.maximum;
    }

    // 查询曝光(exposure)控件
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = V4L2_CID_EXPOSURE;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        exp_min = qctrl.minimum;
        exp_max = qctrl.maximum;
    }

    // 如果有ISO设置请求
    if (m_iso_target != -1) {
        int gain_target = mapRangeReverse(m_iso_target, 100.0, 1600.0, gain_min, gain_max);
        gain_target = std::min(std::max(gain_target, gain_min), gain_max);

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_ANALOGUE_GAIN;
        ctrl.value = gain_target;
        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            std::cout << "[ExposureManager] 已设置 ISO=" << m_iso_target << " (对应 gain=" << gain_target << ")" << std::endl;
        } else {
            std::cerr << "[ExposureManager] 设置 ISO 失败: " << strerror(errno) << std::endl;
        }
        m_iso_target = -1; // 重置请求
    }

    // 如果有EV设置请求
    if (m_ev_target != 999) {
        int exp_target = mapRangeReverse(m_ev_target, -4.0, +4.0, exp_min, exp_max);
        exp_target = std::min(std::max(exp_target, exp_min), exp_max);

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_EXPOSURE;
        ctrl.value = exp_target;
        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            std::cout << "[ExposureManager] 已设置 EV=" << m_ev_target << " (对应 exposure=" << exp_target << ")" << std::endl;
        } else {
            std::cerr << "[ExposureManager] 设置 EV 失败: " << strerror(errno) << std::endl;
        }
        m_ev_target = 999; // 重置请求
    }

    close(fd);
}

