#ifndef CAMERA_SDK_H
#define CAMERA_SDK_H

// 为了在纯 C 代码中使用 bool 类型
#include <stdbool.h>

// __cplusplus 宏只有在 C++ 编译器中才会被定义
// 这段代码的作用是：如果是 C++ 编译器，则用 extern "C" 包裹函数声明，
// 以确保函数名在链接时不会被 C++ 的名字修饰（name mangling）机制改变，
// 从而让 C 语言代码也能正确链接和调用这些函数。
#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化摄像头 SDK 控制器。
     *
     * 这是使用 SDK 的第一步。它会初始化所有必要的内部模块（如 OSD、文件管理等）。
     *
     * @param device_path 摄像头设备的文件路径 (例如 "/dev/video-camera0")。
     * @return 成功时返回一个有效的句柄 (一个不透明指针)，用于后续所有 API 调用。
     * 失败时返回 NULL。
     */
    void *camera_sdk_create(const char *device_path);

    /**
     * @brief 销毁摄像头 SDK 控制器并释放所有资源。
     *
     * 这是程序退出前必须调用的函数，用于确保所有线程被安全停止，所有内存被干净地释放。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     */
    void camera_sdk_destroy(void *handle);

    /**
     * @brief 开始录制视频。
     *
     * 这是一个非阻塞函数。它会启动一个后台线程来执行录制任务，并立即返回。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @param resolution 要录制的分辨率，可以是 "1080p", "720p", 或 "360p"。
     * @return 成功启动返回 0，如果已在录制中或参数错误则返回 -1。
     */
    int camera_sdk_start_recording(void *handle, const char *resolution);

    /**
     * @brief 停止当前正在进行的录制。
     *
     * 这是一个阻塞函数。它会向录制线程发送停止信号，并等待线程完全结束后才返回，
     * 以确保视频文件被正确地写入和关闭。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @return 成功停止返回 0，如果当前没有在录制则返回 -1。
     */
    int camera_sdk_stop_recording(void *handle);

    /**
     * @brief 开始RTSP推流。
     *
     * 这是一个非阻塞函数。它会启动一个后台线程来执行推流任务，并立即返回。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @param url 要推送到的RTSP服务器地址 (例如 "rtsp://localhost:8554/live.stream")。
     * @return 成功启动返回 0，如果已在推流中或参数错误则返回 -1。
     */
    int camera_sdk_start_rtsp_stream(void* handle, const char* url);

    /**
     * @brief 停止当前正在进行的RTSP推流。
     *
     * 这是一个阻塞函数。它会向推流线程发送停止信号，并等待线程完全结束后才返回。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @return 成功停止返回 0，如果当前没有在推流则返回 -1。
     */
    int camera_sdk_stop_rtsp_stream(void* handle);

    /**
     * @brief 拍摄一张快照 (JPEG 图片)。
     *
     * 这是一个非阻塞函数。它会启动一个后台线程来执行拍照任务，并立即返回。
     * 照片文件会被自动保存。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @return 总是返回 0。
     */
    int camera_sdk_take_snapshot(void *handle);

    /**
     * @brief 设置 OSD (On-Screen Display, 屏幕显示) 功能的开关状态。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     * @param enabled true 表示开启 OSD, false 表示关闭 OSD。
     */
    void camera_sdk_set_osd_enabled(void *handle, bool enabled);

    /**
     * @brief 放大 (数字变焦)。
     *
     * 这是一个非阻塞函数，会立即返回。变焦效果将异步应用到后续的录制和拍照中。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     */
    void camera_sdk_zoom_in(void *handle);

    /**
     * @brief 缩小 (数字变焦)。
     *
     * 这是一个非阻塞函数，会立即返回。变焦效果将异步应用到后续的录制和拍照中。
     *
     * @param handle camera_sdk_create 返回的有效句柄。
     */
    void camera_sdk_zoom_out(void *handle);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_SDK_H

