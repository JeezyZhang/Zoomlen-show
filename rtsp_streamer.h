// --- START OF FILE rtsp_streamer.h ---

#ifndef RTSP_STREAMER_H
#define RTSP_STREAMER_H

#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex> // [新增] 包含 mutex

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext.h>
}

#include "osd_manager.h"
#include "zoom_manager.h"
#include "threadsafe_queue.h"

class CameraCapture;

class RtspStreamer {
public:
    RtspStreamer(CameraCapture* capture_module,
                 std::shared_ptr<OsdManager> osd_manager,
                 std::shared_ptr<ZoomManager> zoom_manager);

    ~RtspStreamer();

    bool prepare(const std::string& rtsp_url);
    
    void run();
    void stop();
    bool isStreaming() const;

private:
    void thread_filter_osd();
    void thread_encode_stream();

    bool initialize_ffmpeg();
    void cleanup_ffmpeg();
    bool reconfigure_filters();

    CameraCapture* m_capture_module;
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;

    AVFormatContext *m_ofmt_ctx = nullptr;
    AVCodecContext *m_enc_ctx = nullptr;
    AVFilterGraph *m_filter_graph = nullptr;
    AVFilterContext *m_buffersrc_ctx = nullptr;
    AVFilterContext *m_buffersink_ctx = nullptr;
    
    std::string m_rtsp_url;
    AVStream *m_out_stream = nullptr;
    
    // [新增] 用于消费者内部的时间戳归一化
    int64_t m_first_pts = AV_NOPTS_VALUE;

    bool m_use_hw = false;
    std::atomic<bool> m_stop_flag{false};
    std::atomic<bool> m_is_streaming{false};
    std::atomic<bool> m_pipeline_error{false};

    std::thread m_thread_filter;
    std::thread m_thread_encode;

    // [新增] 用于保护滤镜图重建过程的互斥锁
    std::mutex m_filter_mutex;

    ThreadSafeFrameQueue m_queue_decoded_frames;
    ThreadSafeFrameQueue m_queue_filtered_frames;
};

#endif // RTSP_STREAMER_H```