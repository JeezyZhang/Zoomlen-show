#include "camera_sdk.h" // 甲方唯一需要包含的头文件
#include <iostream>
#include <string>

/**
 * @file example_main.cpp
 * @brief 演示如何使用 camera_sdk.h 中的 API 函数。
 *
 * 这个程序模拟一个最终用户（甲方）的应用程序。它只知道 C 语言接口，
 * 完全不知道内部的 C++ 类实现。
 */

// 打印用户操作提示
void print_usage() {
    std::cout << "\n========= 摄像头 SDK 交互式示例 ==========" << std::endl;
    std::cout << "  record <res>      - 开始录制 (1080p, 720p, 360p)." << std::endl;
    std::cout << "  stop              - 停止当前录制。" << std::endl;
    std::cout << "  stream <rtsp_url> - 开始RTSP推流。" << std::endl;
    std::cout << "  stop_stream       - 停止RTSP推流。" << std::endl;
    std::cout << "  snapshot          - 拍摄一张照片。" << std::endl;
    std::cout << "  osd on/off        - 开启或关闭 OSD。" << std::endl;
    std::cout << "  + / -             - 放大 / 缩小 (步长 0.1x)。" << std::endl;
    std::cout << "  exit              - 退出程序。" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "> ";
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s /dev/videoX\n", argv[0]);
        return -1;
    }
    const char* device_name = argv[1];

    // 1. 初始化 SDK，获取句柄
    void* handle = camera_sdk_create(device_name);
    if (!handle) {
        std::cerr << "SDK 初始化失败，程序退出。" << std::endl;
        return -1;
    }
    std::cout << "SDK 初始化成功。" << std::endl;

    print_usage();
    std::string line;

    // --- 用户命令处理循环 ---
    while (std::getline(std::cin, line)) {
        if (line.rfind("record ", 0) == 0) {
            std::string res = line.substr(7);
            if (camera_sdk_start_recording(handle, res.c_str()) != 0) {
                // SDK 内部会打印错误信息，这里可以不重复打印
            }
        } else if (line == "stop") {
            camera_sdk_stop_recording(handle);
        } else if (line.rfind("stream ", 0) == 0) {
            std::string url = line.substr(7);
            camera_sdk_start_rtsp_stream(handle, url.c_str());
        } else if (line == "stop_stream") {
            camera_sdk_stop_rtsp_stream(handle);
        } else if (line == "snapshot") {
            std::cout << "请求拍照..." << std::endl;
            camera_sdk_take_snapshot(handle);
        } else if (line == "osd on") {
            camera_sdk_set_osd_enabled(handle, true);
        } else if (line == "osd off") {
            camera_sdk_set_osd_enabled(handle, false);
        } else if (line == "+") {
            camera_sdk_zoom_in(handle);
        } else if (line == "-") {
            camera_sdk_zoom_out(handle);
        } else if (line == "exit") {
            break; // 退出命令循环
        } else if (!line.empty()) {
            std::cerr << "未知命令: " << line << std::endl;
        }
        
        std::cout << "> ";
    }

    // --- 程序清理和退出 ---
    std::cout << "正在销毁 SDK，释放所有资源..." << std::endl;
    camera_sdk_destroy(handle);

    std::cout << "程序已干净地退出。" << std::endl;
    return 0;
}

