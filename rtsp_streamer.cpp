// --- START OF FILE rtsp_streamer.cpp ---

#include "rtsp_streamer.h"
#include "app_config.h"
#include "osd_manager.h"
#include "zoom_manager.h"
#include "camera_capture.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <mutex> 
#include <cstring>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h> 
#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h> 
}

static void print_err_rtsp(int ret, const char* context) {
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "[RTSP推流器] FFmpeg 错误 in %s: %s (ret=%d)\n", context, buf, ret);
}

RtspStreamer::RtspStreamer(CameraCapture* capture_module,
                           std::shared_ptr<OsdManager> osd_manager,
                           std::shared_ptr<ZoomManager> zoom_manager)
    : m_capture_module(capture_module),
      m_osd_manager(std::move(osd_manager)),
      m_zoom_manager(std::move(zoom_manager)),
      m_use_hw(false),
      m_stop_flag(false),
      m_is_streaming(false),
      m_pipeline_error(false)
      {}

RtspStreamer::~RtspStreamer()
{
    if (m_is_streaming) {
        stop();
    }
    if (m_thread_filter.joinable()) m_thread_filter.join();
    if (m_thread_encode.joinable()) m_thread_encode.join();
}

bool RtspStreamer::prepare(const std::string& rtsp_url) {
    if (rtsp_url.empty()) {
        std::cerr << "错误: RTSP URL 不能为空。" << std::endl;
        return false;
    }
    m_rtsp_url = rtsp_url;
    return true;
}

void RtspStreamer::stop() { 
    fprintf(stderr, "[RTSP推流器] 收到停止信号...\n");
    m_stop_flag = true;
    m_queue_decoded_frames.stop();
    m_queue_filtered_frames.stop();
}
bool RtspStreamer::isStreaming() const { return m_is_streaming; }

bool RtspStreamer::initialize_ffmpeg()
{
    fprintf(stderr, "[RTSP推流器] 正在连接到 %s (%dx%d)\n", m_rtsp_url.c_str(), RTSP_OUTPUT_WIDTH, RTSP_OUTPUT_HEIGHT);
    int ret = 0;

    AVBufferRef* hw_device_ctx = m_capture_module->get_hw_device_context();
    if (hw_device_ctx != nullptr) {
        fprintf(stderr, "[RTSP推流器] 从采集器获取 RKMPP 硬件设备成功。\n");
        m_use_hw = true;
    } else {
        fprintf(stderr, "[RTSP推流器] 警告: 未获取到 RKMPP 硬件设备, 将回退到纯软件模式。\n");
        m_use_hw = false;
    }

    const AVCodec* enc = avcodec_find_encoder_by_name(RTSP_ENCODER_NAME);
    m_enc_ctx = avcodec_alloc_context3(enc);
    m_enc_ctx->width = RTSP_OUTPUT_WIDTH;
    m_enc_ctx->height = RTSP_OUTPUT_HEIGHT;
    m_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    m_enc_ctx->time_base = {1, 1000000};
    m_enc_ctx->framerate = {30, 1};
    m_enc_ctx->bit_rate = RTSP_BITRATE;
    m_enc_ctx->gop_size = RTSP_GOP_SIZE;
    m_enc_ctx->max_b_frames = 0;

    if (m_use_hw && hw_device_ctx) {
         m_enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }
    if ((ret = avcodec_open2(m_enc_ctx, enc, nullptr)) < 0) {
        print_err_rtsp(ret, "avcodec_open2 for encoder");
        return false;
    }

    avformat_alloc_output_context2(&m_ofmt_ctx, nullptr, "rtsp", m_rtsp_url.c_str());
    if (!m_ofmt_ctx) {
        print_err_rtsp(-1, "avformat_alloc_output_context2 (rtsp)");
        return false;
    }
    if (m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        m_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    m_out_stream = avformat_new_stream(m_ofmt_ctx, nullptr);
    if (!m_out_stream) {
        fprintf(stderr, "[RTSP推流器] 创建输出流失败\n");
        return false;
    }
    avcodec_parameters_from_context(m_out_stream->codecpar, m_enc_ctx);
    m_out_stream->time_base = {1, 90000};

    AVDictionary* rtsp_opts = nullptr;
    av_dict_set(&rtsp_opts, "rtsp_transport", RTSP_TRANSPORT, 0);
    av_dict_set(&rtsp_opts, "muxdelay", "0.1", 0);

    if (!(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&m_ofmt_ctx->pb, m_rtsp_url.c_str(), AVIO_FLAG_WRITE)) < 0) {
            print_err_rtsp(ret, "avio_open (rtsp)");
            av_dict_free(&rtsp_opts);
            return false;
        }
    }

    if ((ret = avformat_write_header(m_ofmt_ctx, &rtsp_opts)) < 0) {
        print_err_rtsp(ret, "avformat_write_header (rtsp)");
        av_dict_free(&rtsp_opts);
        return false;
    }
    av_dict_free(&rtsp_opts);
    printf("[RTSP推流器] RTSP头已写入，推流开始。\n");
    return true;
}

bool RtspStreamer::reconfigure_filters() {
    avfilter_graph_free(&m_filter_graph);
    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph) return false;

    AVCodecContext* dec_ctx = m_capture_module->get_decoder_context();
    AVBufferRef* hw_device_ctx = m_capture_module->get_hw_device_context();
    if (!dec_ctx) {
        fprintf(stderr, "[RTSP推流器] 错误: 无法从采集器获取解码器上下文。\n");
        return false;
    }

    const AVPixelFormat input_pix_fmt = dec_ctx->pix_fmt;
    const bool is_input_hw = (input_pix_fmt == AV_PIX_FMT_DRM_PRIME);

    int cx, cy, cw, ch;
    m_zoom_manager->get_crop_params(cx, cy, cw, ch);

    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    
    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d", 
             dec_ctx->width, dec_ctx->height, input_pix_fmt, 1, 1000000);

    if (is_input_hw && dec_ctx->hw_frames_ctx) {
        char hw_frames_ctx_arg[64];
        snprintf(hw_frames_ctx_arg, sizeof(hw_frames_ctx_arg), ":hw_frames_ctx=%p", (void*)dec_ctx->hw_frames_ctx);
        strcat(args, hw_frames_ctx_arg);
    }
    
    avfilter_graph_create_filter(&m_buffersrc_ctx, buffersrc, "in", args, nullptr, m_filter_graph);
    avfilter_graph_create_filter(&m_buffersink_ctx, buffersink, "out", nullptr, nullptr, m_filter_graph);
    
    enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
    av_opt_set_int_list(m_buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    char filt_descr[1024];
    if (m_use_hw) {
        char rga_part[256];
        snprintf(rga_part, sizeof(rga_part), "vpp_rkrga=cx=%d:cy=%d:cw=%d:ch=%d:w=%d:h=%d",
                 cx, cy, cw, ch, RTSP_OUTPUT_WIDTH, RTSP_OUTPUT_HEIGHT);
        
        if (is_input_hw) {
            fprintf(stderr, "[RTSP推流器] 检测到硬件帧输入(DRM_PRIME)，配置零拷贝滤镜路径。\n");
            snprintf(filt_descr, sizeof(filt_descr), "%s,hwdownload,format=nv12", rga_part);
        } else {
            fprintf(stderr, "[RTSP推流器] 检测到软件帧输入，配置 'hwupload' 滤镜路径。\n");
            snprintf(filt_descr, sizeof(filt_descr), "hwupload,%s,hwdownload,format=nv12", rga_part);
        }
    } else {
         snprintf(filt_descr, sizeof(filt_descr), "crop=%d:%d:%d:%d,scale=%d:%d,format=nv12",
                 cw, ch, cx, cy, RTSP_OUTPUT_WIDTH, RTSP_OUTPUT_HEIGHT);
    }

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_buffersrc_ctx;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_buffersink_ctx;
    int ret = avfilter_graph_parse_ptr(m_filter_graph, filt_descr, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        print_err_rtsp(ret, "avfilter_graph_parse_ptr");
        return false;
    }

    if (m_use_hw && hw_device_ctx) {
        for (unsigned i = 0; i < m_filter_graph->nb_filters; ++i) {
            AVFilterContext* fctx = m_filter_graph->filters[i];
            const char* filter_name = fctx->filter->name;
            if (strcmp(filter_name, "hwupload") == 0 || strcmp(filter_name, "vpp_rkrga") == 0) {
                fctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                fprintf(stderr, "[RTSP推流器] 已绑定 hw_device_ctx 到 %s 滤镜\n", filter_name);
            }
        }
    }

    if ((ret = avfilter_graph_config(m_filter_graph, nullptr)) < 0) {
        print_err_rtsp(ret, "avfilter_graph_config");
        return false;
    }

    fprintf(stderr, "[RTSP推流器] 滤镜图配置完成: \"%s\"\n", filt_descr);
    return true;
}

void RtspStreamer::run() {
    m_is_streaming = true;
    m_pipeline_error = false;
    m_stop_flag = false;

    if (!initialize_ffmpeg()) {
        fprintf(stderr, "[RTSP推流器] 错误: initialize_ffmpeg 失败\n");
        cleanup_ffmpeg();
        m_is_streaming = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        if (!reconfigure_filters()) {
            fprintf(stderr, "[RTSP推流器] 错误: 首次配置滤镜图失败\n");
            cleanup_ffmpeg();
            m_is_streaming = false;
            return;
        }
    }

    m_capture_module->register_consumer(&m_queue_decoded_frames);

    fprintf(stderr, "[RTSP推流器] 启动流水线线程...\n");
    try {
        m_thread_filter = std::thread(&RtspStreamer::thread_filter_osd, this);
        m_thread_encode = std::thread(&RtspStreamer::thread_encode_stream, this);
    } catch (const std::exception& e) {
        fprintf(stderr, "[RTSP推流器] 启动线程失败: %s\n", e.what());
        m_pipeline_error = true;
        stop();
    }

    if (m_thread_filter.joinable()) m_thread_filter.join();
    if (m_thread_encode.joinable()) m_thread_encode.join();

    fprintf(stderr, "[RTSP推流器] 流水线线程已全部退出。\n");

    m_capture_module->unregister_consumer(&m_queue_decoded_frames);

    cleanup_ffmpeg();
    fprintf(stderr, "[RTSP推流器] 推流结束。\n");
    m_is_streaming = false;
}

void RtspStreamer::cleanup_ffmpeg() {
    fprintf(stderr, "[RTSP推流器] 正在清理 FFmpeg 资源...\n");

    if (m_enc_ctx && m_ofmt_ctx && m_out_stream) {
        AVPacket* outpkt = av_packet_alloc();
        if (avcodec_send_frame(m_enc_ctx, nullptr) >= 0) {
            while (avcodec_receive_packet(m_enc_ctx, outpkt) >= 0) {
                av_packet_rescale_ts(outpkt, m_enc_ctx->time_base, m_out_stream->time_base);
                outpkt->stream_index = m_out_stream->index;
                av_interleaved_write_frame(m_ofmt_ctx, outpkt);
                av_packet_unref(outpkt);
            }
        }
        av_packet_free(&outpkt);
        av_write_trailer(m_ofmt_ctx);
    }
    
    if (m_ofmt_ctx && !(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m_ofmt_ctx->pb);
    if (m_ofmt_ctx) avformat_free_context(m_ofmt_ctx);
    if (m_enc_ctx) avcodec_free_context(&m_enc_ctx);
    
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        avfilter_graph_free(&m_filter_graph);
        m_filter_graph = nullptr;
    }

    m_queue_decoded_frames.clear();
    m_queue_filtered_frames.clear();
    
    m_ofmt_ctx = nullptr;
    m_enc_ctx = nullptr;
    m_out_stream = nullptr;
}

void RtspStreamer::thread_filter_osd()
{
    fprintf(stderr, "[T1:Filter-RTSP] 滤镜OSD线程启动。\n");
    AVFrame *filt_frame = av_frame_alloc();

    while (!m_stop_flag && !m_pipeline_error) {
        AVFramePtr frame_ptr = m_queue_decoded_frames.wait_and_pop();
        if (frame_ptr == nullptr) {
            break;
        }

        // [修复] 时间戳归一化
        AVFrame* frame = frame_ptr.get();
        if (m_first_pts == AV_NOPTS_VALUE) {
            m_first_pts = frame->pts;
        }
        frame->pts -= m_first_pts;

        if (m_zoom_manager && m_zoom_manager->check_and_reset_change_flag()) {
            fprintf(stderr, "[T1:Filter-RTSP] 检测到变焦，正在动态重建滤镜图...\n");
            
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            if (!reconfigure_filters()) {
                fprintf(stderr, "[T1:Filter-RTSP] 错误: 动态重建滤镜失败，正在停止推流。\n");
                m_pipeline_error = true;
                break;
            }
            fprintf(stderr, "[T1:Filter-RTSP] 滤镜图已成功更新。\n");
        }
        
        {
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            if (m_pipeline_error || !m_buffersrc_ctx) {
                continue;
            }
            if (av_buffersrc_add_frame_flags(m_buffersrc_ctx, frame, 0) < 0) {
                fprintf(stderr, "[T1:Filter-RTSP] 错误: av_buffersrc_add_frame 失败\n");
                m_pipeline_error = true;
                break;
            }
        }

        while (!m_stop_flag && !m_pipeline_error) {
            int ret = 0;
            {
                std::lock_guard<std::mutex> lock(m_filter_mutex);
                 if (m_pipeline_error || !m_buffersink_ctx) {
                    ret = AVERROR_EOF;
                } else {
                    ret = av_buffersink_get_frame(m_buffersink_ctx, filt_frame);
                }
            }

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                print_err_rtsp(ret, "av_buffersink_get_frame");
                m_pipeline_error = true;
                break;
            }

            if (m_osd_manager) {
                m_osd_manager->blend_osd_on_frame(filt_frame);
            }

            AVFrame* filt_frame_copy = av_frame_clone(filt_frame);
            if (!filt_frame_copy) {
                 fprintf(stderr, "[T1:Filter-RTSP] 错误: av_frame_clone (filt) 失败\n");
                 m_pipeline_error = true;
                 break;
            }

            m_queue_filtered_frames.push(make_avframe_ptr(filt_frame_copy));
            av_frame_unref(filt_frame);
        }
    }

    av_frame_free(&filt_frame);
    m_queue_filtered_frames.stop();
    fprintf(stderr, "[T1:Filter-RTSP] 滤镜OSD线程退出。\n");
}

void RtspStreamer::thread_encode_stream()
{
    fprintf(stderr, "[T2:Encode-RTSP] 编码推流线程启动。\n");
    AVPacket* outpkt = av_packet_alloc();
    
    while (!m_stop_flag && !m_pipeline_error) {
        AVFramePtr frame_ptr = m_queue_filtered_frames.wait_and_pop();
        if (frame_ptr == nullptr) {
            break;
        }

        AVFrame* frame = frame_ptr.get();
        if (frame->pts != AV_NOPTS_VALUE) {
            frame->pts = av_rescale_q(frame->pts, m_buffersink_ctx->inputs[0]->time_base, m_enc_ctx->time_base);
        }

        int ret = avcodec_send_frame(m_enc_ctx, frame);
        if (ret < 0) {
            print_err_rtsp(ret, "avcodec_send_frame (encoder)");
            m_pipeline_error = true;
            break;
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(m_enc_ctx, outpkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                print_err_rtsp(ret, "avcodec_receive_packet (encoder)");
                m_pipeline_error = true;
                break;
            }

            av_packet_rescale_ts(outpkt, m_enc_ctx->time_base, m_out_stream->time_base);
            outpkt->stream_index = m_out_stream->index;
            
            ret = av_interleaved_write_frame(m_ofmt_ctx, outpkt);
            av_packet_unref(outpkt);
            if (ret < 0) {
                print_err_rtsp(ret, "av_interleaved_write_frame (rtsp)");
                m_pipeline_error = true;
                break;
            }
        }
    }

    av_packet_free(&outpkt);
    m_queue_filtered_frames.stop();
    fprintf(stderr, "[T2:Encode-RTSP] 编码推流线程退出。\n");
}