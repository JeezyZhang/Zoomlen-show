#include "camera_sdk.h" // 甲方唯一需要包含的头文件
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <stdexcept> // For std::stoi, std::stod exceptions
#include <atomic>    // [修复] 包含 <atomic> 头文件

/**
 * @file example_main.cpp
 * @brief 演示如何使用 camera_sdk.h 中的 API 函数。
 *
 * 这个程序模拟一个最终用户（甲方）的应用程序。它只知道 C 语言接口，
 * 完全不知道内部的 C++ 类实现。
 */

// 用于控制OSD更新线程的全局原子变量
std::atomic<bool> g_run_osd_thread(true);

// OSD数据更新线程函数
void osd_update_thread_func(void *handle)
{
    if (!handle)
        return;

    camera_sdk_pos_data_t osd_data;
    osd_data.latitude = 22.5430;
    osd_data.longitude = 114.0578;
    osd_data.speed_kmh = 50.0;

    char time_buffer[100];

    while (g_run_osd_thread)
    {
        // 模拟数据更新
        osd_data.latitude += 0.00001;
        osd_data.longitude += 0.00002;
        osd_data.speed_kmh = 50.0f + (rand() % 200 - 100) / 10.0f;

        // 获取并格式化当前时间
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %X", std::localtime(&in_time_t));
        osd_data.timestamp = time_buffer;

        // 调用SDK接口设置OSD数据
        camera_sdk_set_osd_data(handle, &osd_data);

        // 每秒更新一次
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// 打印用户操作提示
void print_usage()
{
    std::cout << "\n========= 摄像头 SDK 交互式示例 ==========" << std::endl;
    std::cout << "  record <res>      - 开始录制 (1080p, 720p, 360p)." << std::endl;
    std::cout << "  stop              - 停止当前录制。" << std::endl;
    std::cout << "  stream <rtsp_url> - 开始RTSP推流。" << std::endl;
    std::cout << "  stop_stream       - 停止RTSP推流。" << std::endl;
    std::cout << "  snapshot          - 拍摄一张照片。" << std::endl;
    std::cout << "  osd on/off        - 开启或关闭 OSD。" << std::endl;
    std::cout << "  + / -             - 放大 / 缩小 (步长 0.1x)。" << std::endl;
    std::cout << "  iso <value>       - 设置 ISO (例如: iso 800)。" << std::endl;
    std::cout << "  ev <value>        - 设置 EV (例如: ev -1.0)。" << std::endl;
    std::cout << "  exit              - 退出程序。" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "> ";
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "用法: %s /dev/videoX\n", argv[0]);
        return -1;
    }
    const char *device_name = argv[1];

    // 1. 初始化 SDK，获取句柄
    void *handle = camera_sdk_create(device_name);
    if (!handle)
    {
        std::cerr << "SDK 初始化失败，程序退出。" << std::endl;
        return -1;
    }
    std::cout << "SDK 初始化成功。" << std::endl;

    // 2. 启动一个独立的线程来模拟外部设备持续传入OSD数据
    std::thread osd_thread(osd_update_thread_func, handle);

    print_usage();
    std::string line;

    // --- 用户命令处理循环 ---
    while (std::getline(std::cin, line))
    {
        if (line.rfind("record ", 0) == 0)
        {
            std::string res = line.substr(7);
            if (camera_sdk_start_recording(handle, res.c_str()) != 0)
            {
                // SDK 内部会打印错误信息，这里可以不重复打印
            }
        }
        else if (line == "stop")
        {
            camera_sdk_stop_recording(handle);
        }
        else if (line.rfind("stream ", 0) == 0)
        {
            std::string url = line.substr(7);
            camera_sdk_start_rtsp_stream(handle, url.c_str());
        }
        else if (line == "stop_stream")
        {
            camera_sdk_stop_rtsp_stream(handle);
        }
        else if (line == "snapshot")
        {
            camera_sdk_take_snapshot(handle);
        }
        else if (line == "osd on")
        {
            camera_sdk_set_osd_enabled(handle, true);
        }
        else if (line == "osd off")
        {
            camera_sdk_set_osd_enabled(handle, false);
        }
        else if (line == "+")
        {
            camera_sdk_zoom_in(handle);
        }
        else if (line == "-")
        {
            camera_sdk_zoom_out(handle);
        }
        else if (line.rfind("iso ", 0) == 0)
        {
            try
            {
                int iso = std::stoi(line.substr(4));
                camera_sdk_set_iso(handle, iso);
            }
            catch (const std::exception &e)
            {
                std::cerr << "无效的 ISO 值: " << line.substr(4) << std::endl;
            }
        }
        else if (line.rfind("ev ", 0) == 0)
        {
            try
            {
                double ev = std::stod(line.substr(3));
                camera_sdk_set_ev(handle, ev);
            }
            catch (const std::exception &e)
            {
                std::cerr << "无效的 EV 值: " << line.substr(3) << std::endl;
            }
        }
        else if (line == "exit")
        {
            break; // 退出命令循环
        }
        else if (!line.empty())
        {
            std::cerr << "未知命令: " << line << std::endl;
        }

        std::cout << "> ";
    }

    // --- 程序清理和退出 ---
    // 通知OSD线程退出
    g_run_osd_thread = false;
    if (osd_thread.joinable())
    {
        osd_thread.join();
    }

    std::cout << "正在销毁 SDK，释放所有资源..." << std::endl;
    camera_sdk_destroy(handle);

    std::cout << "程序已干净地退出。" << std::endl;
    return 0;
}
