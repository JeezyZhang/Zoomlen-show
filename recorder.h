#ifndef RECORDER_H
#define RECORDER_H

#include <string>
#include <functional>
#include <atomic>
#include <map>
#include <memory>

// 向前声明
class OsdManager;
class ZoomManager;

// 媒体文件处理完成后的回调函数类型
using MediaCompleteCallback = std::function<void(const std::string &)>;

/*
 * Recorder 类
 * 负责视频录制的完整生命周期管理
 */
class Recorder
{
public:
    // 构造函数
    Recorder(std::string device,
             std::shared_ptr<OsdManager> osd_manager,
             std::shared_ptr<ZoomManager> zoom_manager,
             MediaCompleteCallback cb);

    bool prepare(const std::string &resolution_key);
    void stop();
    bool isRecording() const;
    void run();

private:
    void cleanup();
    // 新增: 用于初始化或重建滤镜图的辅助函数
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
