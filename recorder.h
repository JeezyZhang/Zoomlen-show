// --- START OF FILE recorder.h ---

#ifndef RECORDER_H
#define RECORDER_H

#include <atomic>
#include <memory>
#include <string>
#include <map>
#include <functional>
#include <thread>
#include <mutex>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include "osd_manager.h"
#include "zoom_manager.h"
#include "threadsafe_queue.h"

class CameraCapture;

using MediaCompleteCallback = std::function<void(const std::string &)>;

class Recorder
{
public:
    Recorder(CameraCapture* capture_module,
             std::shared_ptr<OsdManager> osd_manager,
             std::shared_ptr<ZoomManager> zoom_manager,
             MediaCompleteCallback cb);
    
    ~Recorder();

    bool prepare(const std::string &resolution_key);
    
    void run();
    void stop();
    bool isRecording() const;

private:
    void thread_filter_osd();
    void thread_encode_write();

    bool initialize_ffmpeg();
    void cleanup_ffmpeg();
    bool reconfigure_filters();

    CameraCapture* m_capture_module;
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    MediaCompleteCallback m_on_complete_cb;
    
    AVFormatContext *m_ofmt_ctx = nullptr;
    AVCodecContext *m_enc_ctx = nullptr;
    AVFilterGraph *m_filter_graph = nullptr;
    AVFilterContext *m_buffersrc_ctx = nullptr;
    AVFilterContext *m_buffersink_ctx = nullptr;
    
    std::string m_out_filename;
    int m_out_w = 0, m_out_h = 0;
    AVStream *m_out_stream = nullptr;

    // [新增] 用于消费者内部的时间戳归一化
    int64_t m_first_pts = AV_NOPTS_VALUE;

    bool m_use_hw = false;
    std::atomic<bool> m_stop_flag{false};
    std::atomic<bool> m_is_recording{false};
    std::atomic<bool> m_pipeline_error{false};

    std::thread m_thread_filter;
    std::thread m_thread_encode;

    std::mutex m_filter_mutex;

    ThreadSafeFrameQueue m_queue_decoded_frames;
    ThreadSafeFrameQueue m_queue_filtered_frames;
};

#endif // RECORDER_H