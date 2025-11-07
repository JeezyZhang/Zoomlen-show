// --- START OF FILE camera_capture.h ---

#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <list>
#include <future>
#include <memory>
#include "threadsafe_queue.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

extern std::mutex g_camera_device_mutex;

class ZoomManager;

class CameraCapture
{
public:
    CameraCapture(std::string device_path);
    ~CameraCapture();

    bool start();
    void stop();

    void register_consumer(ThreadSafeFrameQueue* consumer_queue);
    void unregister_consumer(ThreadSafeFrameQueue* consumer_queue);

    AVBufferRef* get_hw_device_context() const { return m_hw_device_ctx; }
    AVCodecContext* get_decoder_context() const { return m_input_codec_ctx; }

    std::future<AVFramePtr> request_single_frame();

private:
    void capture_loop();
    bool initialize_ffmpeg();
    void cleanup_ffmpeg();
    void fan_out_frame(AVFrame* frame);

    std::string m_device_path;

    std::thread m_capture_thread;
    std::atomic<bool> m_stop_flag{false};
    std::atomic<bool> m_is_running{false};

    AVFormatContext* m_ifmt_ctx = nullptr;
    AVCodecContext* m_input_codec_ctx = nullptr;
    AVBufferRef* m_hw_device_ctx = nullptr;

    int64_t m_first_pts = AV_NOPTS_VALUE;

    std::list<ThreadSafeFrameQueue*> m_consumers;
    std::mutex m_consumer_mutex;

    std::list<std::promise<AVFramePtr>> m_single_frame_requests;
    std::mutex m_request_mutex;
};

#endif // CAMERA_CAPTURE_H