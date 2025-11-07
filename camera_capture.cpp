// --- START OF FILE camera_capture.cpp ---

#include "camera_capture.h"
#include "app_config.h"
#include "zoom_manager.h"

#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

static void print_err_capture(int ret, const char* context) {
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "[CameraCapture] FFmpeg 错误 in %s: %s (ret=%d)\n", context, buf, ret);
}

CameraCapture::CameraCapture(std::string device_path)
    : m_device_path(std::move(device_path)) {}

CameraCapture::~CameraCapture() {
    stop();
}

bool CameraCapture::start() {
    if (m_is_running) {
        fprintf(stderr, "[CameraCapture] 已经启动。\n");
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_camera_device_mutex);
        if (!initialize_ffmpeg()) {
            fprintf(stderr, "[CameraCapture] 错误: initialize_ffmpeg 失败\n");
            cleanup_ffmpeg();
            return false;
        }
    }

    m_stop_flag = false;
    m_is_running = true;
    try {
        m_capture_thread = std::thread(&CameraCapture::capture_loop, this);
    } catch (const std::exception& e) {
        fprintf(stderr, "[CameraCapture] 启动线程失败: %s\n", e.what());
        m_is_running = false;
        cleanup_ffmpeg();
        return false;
    }

    fprintf(stderr, "[CameraCapture] 采集模块启动成功。\n");
    return true;
}

void CameraCapture::stop() {
    if (!m_is_running.exchange(false)) {
        return;
    }

    fprintf(stderr, "[CameraCapture] 收到停止信号...\n");
    m_stop_flag = true;

    {
        std::lock_guard<std::mutex> lock(m_consumer_mutex);
        for (auto* q : m_consumers) {
            q->stop();
        }
    }

    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }

    fprintf(stderr, "[CameraCapture] 采集线程已退出。\n");
    cleanup_ffmpeg();
    fprintf(stderr, "[CameraCapture] 模块已停止并清理。\n");
}

bool CameraCapture::initialize_ffmpeg() {
    fprintf(stderr, "[CameraCapture] 正在初始化 FFmpeg...\n");
    int ret = 0;
    m_first_pts = AV_NOPTS_VALUE;

    ret = av_hwdevice_ctx_create(&m_hw_device_ctx, AV_HWDEVICE_TYPE_RKMPP, nullptr, nullptr, 0);
    if (ret < 0) {
        print_err_capture(ret, "av_hwdevice_ctx_create (RKMPP)");
        fprintf(stderr, "[CameraCapture] 警告: 创建 RKMPP 硬件设备失败。硬件加速将不可用。\n");
    } else {
        fprintf(stderr, "[CameraCapture] 创建 RKMPP 硬件设备成功。\n");
    }

    AVDictionary* opts = nullptr;
    char video_size_str[64];
    snprintf(video_size_str, sizeof(video_size_str), "%dx%d", V4L2_INPUT_WIDTH, V4L2_INPUT_HEIGHT);
    
    av_dict_set(&opts, "input_format", "nv12", 0); 
    av_dict_set(&opts, "framerate", "30", 0);
    av_dict_set(&opts, "video_size", video_size_str, 0);

    const AVInputFormat* iformat = av_find_input_format("v4l2");
    if ((ret = avformat_open_input(&m_ifmt_ctx, m_device_path.c_str(), iformat, &opts)) < 0) {
        print_err_capture(ret, "avformat_open_input");
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    if ((ret = avformat_find_stream_info(m_ifmt_ctx, nullptr)) < 0) {
        print_err_capture(ret, "avformat_find_stream_info");
        return false;
    }

    int video_stream_index = av_find_best_stream(m_ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        fprintf(stderr, "[CameraCapture] 找不到视频流\n");
        return false;
    }

    AVStream* in_video_stream = m_ifmt_ctx->streams[video_stream_index];
    
    m_input_codec_ctx = avcodec_alloc_context3(nullptr);
    if (!m_input_codec_ctx) {
        fprintf(stderr, "[CameraCapture] avcodec_alloc_context3 (params) 失败\n");
        return false;
    }
    if(avcodec_parameters_to_context(m_input_codec_ctx, in_video_stream->codecpar) < 0) {
        fprintf(stderr, "[CameraCapture] avcodec_parameters_to_context 失败\n");
        return false;
    }
    // [修正] 将帧率信息也保存到上下文中，供消费者使用
    m_input_codec_ctx->framerate = in_video_stream->r_frame_rate;


    fprintf(stderr, "[CameraCapture] 成功打开 V4L2 设备，输入格式为: %s\n", av_get_pix_fmt_name(m_input_codec_ctx->pix_fmt));
    return true;
}

void CameraCapture::cleanup_ffmpeg() {
    fprintf(stderr, "[CameraCapture] 正在清理 FFmpeg 资源...\n");
    
    if (m_input_codec_ctx) avcodec_free_context(&m_input_codec_ctx);
    if (m_ifmt_ctx) avformat_close_input(&m_ifmt_ctx);
    if (m_hw_device_ctx) av_buffer_unref(&m_hw_device_ctx);

    m_ifmt_ctx = nullptr;
    m_input_codec_ctx = nullptr;
    m_hw_device_ctx = nullptr;
}

void CameraCapture::capture_loop() {
    fprintf(stderr, "[CaptureLoop] 采集线程启动。\n");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* raw_frame = av_frame_alloc();
    int ret = 0;

    if (!pkt || !raw_frame) {
        fprintf(stderr, "[CaptureLoop] 错误: 无法分配 pkt 或 frame\n");
        m_stop_flag = true;
    }

    raw_frame->width = m_input_codec_ctx->width;
    raw_frame->height = m_input_codec_ctx->height;
    raw_frame->format = m_input_codec_ctx->pix_fmt;
    if (av_frame_get_buffer(raw_frame, 0) < 0) {
        fprintf(stderr, "[CaptureLoop] 错误: 无法为原始帧分配缓冲区\n");
        m_stop_flag = true;
    }

    while (!m_stop_flag) {
        ret = av_read_frame(m_ifmt_ctx, pkt);
        if (ret < 0) {
            print_err_capture(ret, "av_read_frame");
            break;
        }

        uint8_t *src_data[4] = { nullptr };
        int src_linesize[4] = { 0 };

        ret = av_image_fill_arrays(src_data, src_linesize, pkt->data,
                                   (AVPixelFormat)raw_frame->format, raw_frame->width, raw_frame->height, 1);
        if (ret < 0) {
            print_err_capture(ret, "av_image_fill_arrays");
            av_packet_unref(pkt);
            continue;
        }

        av_image_copy(raw_frame->data, raw_frame->linesize,
                      (const uint8_t**)src_data, src_linesize,
                      (AVPixelFormat)raw_frame->format, raw_frame->width, raw_frame->height);

        if (m_first_pts == AV_NOPTS_VALUE) {
            m_first_pts = pkt->pts;
        }
        raw_frame->pts = (pkt->pts != AV_NOPTS_VALUE) ? (pkt->pts - m_first_pts) : 0;
        if (raw_frame->pts < 0) raw_frame->pts = 0;
        
        {
            std::lock_guard<std::mutex> lock(m_request_mutex);
            if (!m_single_frame_requests.empty()) {
                AVFrame* frame_for_request = av_frame_clone(raw_frame);
                if (frame_for_request) {
                    m_single_frame_requests.front().set_value(make_avframe_ptr(frame_for_request));
                } else {
                    m_single_frame_requests.front().set_value(nullptr);
                }
                m_single_frame_requests.pop_front();
            }
        }

        fan_out_frame(raw_frame);
        
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&raw_frame);
    
    {
        std::lock_guard<std::mutex> lock(m_consumer_mutex);
        for (auto* q : m_consumers) {
            q->stop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(m_request_mutex);
        for (auto& promise : m_single_frame_requests) {
            promise.set_value(nullptr);
        }
        m_single_frame_requests.clear();
    }
    
    fprintf(stderr, "[CaptureLoop] 采集线程退出。\n");
}

void CameraCapture::fan_out_frame(AVFrame* frame) {
    std::lock_guard<std::mutex> lock(m_consumer_mutex);

    if (m_consumers.empty()) {
        return;
    }

    AVFrame* frame_to_distribute = av_frame_clone(frame);
    if (!frame_to_distribute) {
        fprintf(stderr, "[CameraCapture] 错误: av_frame_clone 失败，无法分发帧。\n");
        return;
    }
    
    AVFramePtr frame_ptr = make_avframe_ptr(frame_to_distribute);

    for (auto* consumer_queue : m_consumers) {
        consumer_queue->push(frame_ptr);
    }
}

std::future<AVFramePtr> CameraCapture::request_single_frame() {
    std::promise<AVFramePtr> promise;
    std::future<AVFramePtr> future = promise.get_future();
    
    std::lock_guard<std::mutex> lock(m_request_mutex);
    m_single_frame_requests.push_back(std::move(promise));
    
    return future;
}

void CameraCapture::register_consumer(ThreadSafeFrameQueue* consumer_queue) {
    if (!consumer_queue) return;
    
    std::lock_guard<std::mutex> lock(m_consumer_mutex);
    m_consumers.push_back(consumer_queue);
    fprintf(stderr, "[CameraCapture] 注册了一个新消费者。当前总数: %zu\n", m_consumers.size());
}

void CameraCapture::unregister_consumer(ThreadSafeFrameQueue* consumer_queue) {
    if (!consumer_queue) return;

    std::lock_guard<std::mutex> lock(m_consumer_mutex);
    m_consumers.remove(consumer_queue);
    fprintf(stderr, "[CameraCapture] 注销了一个消费者。剩余总数: %zu\n", m_consumers.size());
}