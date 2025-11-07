// --- START OF FILE snapshotter.h ---

#ifndef SNAPSHOTTER_H
#define SNAPSHOTTER_H

#include <string>
#include <functional>
#include <memory>

// 包含 Snapshotter 内部需要的 FFmpeg 头文件
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>
}

class OsdManager;
class ZoomManager;
class CameraCapture;

struct AVFrame;

// 需要从 threadsafe_queue.h 引入 AVFramePtr
#include "threadsafe_queue.h" 

using MediaCompleteCallback = std::function<void(const std::string&)>;

class Snapshotter {
public:
    Snapshotter(CameraCapture* capture_module,
                std::shared_ptr<OsdManager> osd_manager,
                std::shared_ptr<ZoomManager> zoom_manager,
                MediaCompleteCallback cb);
    ~Snapshotter();

    void run();

private:
    bool setup_filter_graph(AVFrame* in_frame);
    void cleanup_filter_graph();

    CameraCapture* m_capture_module;
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    MediaCompleteCallback m_on_complete_cb;
    
    AVFilterGraph *m_filter_graph = nullptr;
    AVFilterContext *m_buffersrc_ctx = nullptr;
    AVFilterContext *m_buffersink_ctx = nullptr;
};

#endif // SNAPSHOTTER_H