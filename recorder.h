#ifndef RECORDER_H
#define RECORDER_H

#include <string>
#include <functional>
#include <atomic>
#include <map>
#include <memory>

// 向前声明，避免在头文件中互相包含
class OsdManager;
class ZoomManager;

// 定义一个回调函数类型，用于在媒体文件处理完成后通知上层
using MediaCompleteCallback = std::function<void(const std::string&)>;

/**
 * @class Recorder
 * @brief 负责视频录制的完整生命周期管理。
 * * 在一个独立的线程中完成所有 FFmpeg 操作，以避免阻塞主线程。
 * 能够与 OsdManager 和 ZoomManager 协作，实现 OSD 和变焦功能。
 */
class Recorder {
public:
    /**
     * @brief 构造函数。
     * @param device 摄像头设备路径 (例如 "/dev/video0")。
     * @param osd_manager OSD 管理器的共享指针。
     * @param zoom_manager 变焦管理器的共享指针。
     * @param cb 录制完成后要调用的回调函数。
     */
    Recorder(std::string device,
             std::shared_ptr<OsdManager> osd_manager,
             std::shared_ptr<ZoomManager> zoom_manager,
             MediaCompleteCallback cb);

    /**
     * @brief 准备录制。
     * @param resolution_key "1080p", "720p", 或 "360p"。
     * @return 准备成功返回 true，否则返回 false。
     */
    bool prepare(const std::string& resolution_key);
    
    /**
     * @brief 请求停止录制 (线程安全)。
     * * 从外部线程调用此函数，向录制线程发送停止信号。
     */
    void stop();

    /**
     * @brief 检查当前是否正在录制。
     */
    bool isRecording() const;

    /**
     * @brief 核心录制函数，应在一个独立的线程中运行。
     */
    void run();

private:
    // 清理所有 FFmpeg 相关资源
    void cleanup();
    
    /**
     * @brief 初始化或根据新的变焦参数重建 FFmpeg 滤镜图。
     * @return 成功返回 true，失败返回 false。
     */
    bool reconfigure_filters();

    // FFmpeg 上下文
    struct AVFormatContext *m_ifmt_ctx = nullptr;
    struct AVFormatContext *m_ofmt_ctx = nullptr;
    struct AVCodecContext *m_dec_ctx = nullptr;
    struct AVCodecContext *m_enc_ctx = nullptr;
    struct AVFilterGraph *m_filter_graph = nullptr;
    struct AVFilterContext *m_buffersrc_ctx = nullptr;
    struct AVFilterContext *m_buffersink_ctx = nullptr;
    struct AVBufferRef *m_hw_device_ctx = nullptr;
    
    // 状态和控制标志
    std::atomic<bool> m_stop_flag{false};
    std::atomic<bool> m_is_recording{false};

    // 参数
    std::string m_device_name;
    std::string m_out_filename;
    int m_out_w = 0, m_out_h = 0;

    // 管理器和回调
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    MediaCompleteCallback m_on_complete_cb;
};

#endif // RECORDER_H

