#ifndef EXPOSURE_MANAGER_H
#define EXPOSURE_MANAGER_H

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @class ExposureManager
 * @brief 负责手动设置摄像头的曝光(EV)和增益(ISO)。
 *
 * 在一个独立的后台线程中运行，以异步方式处理设置请求，避免阻塞主线程。
 */
class ExposureManager {
public:
    ExposureManager(std::string device_path);
    ~ExposureManager();

    // 启动后台线程
    void start();

    // 停止后台线程
    void stop();

    /**
     * @brief 设置目标ISO值。
     * @param iso 要设置的ISO值 (例如 100, 200, 400, ...)。
     */
    void set_iso(int iso);

    /**
     * @brief 设置目标EV（曝光补偿）值。
     * @param ev 要设置的EV值 (例如 -2.0, -1.0, 0, 1.0, 2.0, ...)。
     */
    void set_ev(double ev);

private:
    // 线程主函数
    void run();

    // 实际执行V4L2 ioctl操作的函数
    void apply_settings();

    // V4L2设备路径
    std::string m_device_path;

    // 线程与同步相关
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop_flag{false};

    // 待处理的设置请求
    int m_iso_target = -1;  // -1表示无新请求
    double m_ev_target = 999; // 999表示无新请求
    bool m_new_request = false;
};

#endif // EXPOSURE_MANAGER_H

