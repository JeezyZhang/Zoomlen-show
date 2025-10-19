#ifndef RTSP_STREAMER_H
#define RTSP_STREAMER_H

#include <string>
#include <functional>
#include <atomic>
#include <memory>

// 前向声明
class OsdManager;
class ZoomManager;
struct AVFormatContext;
struct AVCodecContext;
struct AVFilterGraph;
struct AVFilterContext;
struct AVBufferRef;

/**
 * @class RtspStreamer
 * @brief 管理RTSP推流的整个生命周期。
 *
 * 此类将所有用于推流的FFmpeg操作封装在一个独立的线程中，
 * 以避免阻塞主应用程序逻辑。它与OsdManager和ZoomManager协作，
 * 以在推流过程中实现OSD叠加和数字变焦功能。
 */
class RtspStreamer {
public:
    /**
     * @brief 构造函数。
     * @param device 摄像头设备路径 (例如 "/dev/video0")。
     * @param osd_manager 指向OsdManager的共享指针。
     * @param zoom_manager 指向ZoomManager的共享指针。
     */
    RtspStreamer(std::string device,
                 std::shared_ptr<OsdManager> osd_manager,
                 std::shared_ptr<ZoomManager> zoom_manager);

    /**
     * @brief 为给定的RTSP URL准备推流。
     * @param rtsp_url 目标RTSP URL。
     * @return 成功返回 true，否则返回 false。
     */
    bool prepare(const std::string& rtsp_url);

    /**
     * @brief 请求停止推流 (线程安全)。
     */
    void stop();

    /**
     * @brief 检查当前是否正在推流。
     */
    bool isStreaming() const;

    /**
     * @brief 核心推流函数，应在一个独立的线程中运行。
     */
    void run();

private:
    // 清理所有FFmpeg相关资源
    void cleanup();

    // 根据新的变焦参数重新配置FFmpeg滤镜图
    bool reconfigure_filters();

    // FFmpeg 上下文
    AVFormatContext *m_ifmt_ctx = nullptr;
    AVFormatContext *m_ofmt_ctx = nullptr;
    AVCodecContext *m_dec_ctx = nullptr;
    AVCodecContext *m_enc_ctx = nullptr;
    AVFilterGraph *m_filter_graph = nullptr;
    AVFilterContext *m_buffersrc_ctx = nullptr;
    AVFilterContext *m_buffersink_ctx = nullptr;
    AVBufferRef *m_hw_device_ctx = nullptr;
    
    // 状态和控制标志
    std::atomic<bool> m_stop_flag{false};
    std::atomic<bool> m_is_streaming{false};

    // 参数
    std::string m_device_name;
    std::string m_rtsp_url;

    // 管理器
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
};

#endif // RTSP_STREAMER_H

