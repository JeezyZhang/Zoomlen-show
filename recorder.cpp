// --- START OF FILE recorder.cpp ---

#include "recorder.h"
#include "app_config.h"
#include "osd_manager.h"
#include "zoom_manager.h"
#include "camera_capture.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <cstring>

extern "C"
{
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

static void print_err(int ret, const char *context)
{
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "[录制器] FFmpeg 错误 in %s: %s (ret=%d)\n", context, buf, ret);
}

static std::string generate_timestamp_filename()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d%H%M%S");
    return ss.str() + ".mp4";
}

const std::map<std::string, std::pair<int, int>> resolutions = {
    {"1080p", {1920, 1080}},
    {"720p", {1280, 720}},
    {"360p", {640, 360}}};

Recorder::Recorder(CameraCapture* capture_module,
                   std::shared_ptr<OsdManager> osd_manager,
                   std::shared_ptr<ZoomManager> zoom_manager,
                   MediaCompleteCallback cb)
    : m_capture_module(capture_module),
      m_osd_manager(std::move(osd_manager)),
      m_zoom_manager(std::move(zoom_manager)),
      m_on_complete_cb(std::move(cb)),
      m_use_hw(false),
      m_stop_flag(false),
      m_is_recording(false),
      m_pipeline_error(false)
{
}

Recorder::~Recorder()
{
    if (m_is_recording) {
        stop();
    }
    if (m_thread_filter.joinable()) m_thread_filter.join();
    if (m_thread_encode.joinable()) m_thread_encode.join();
}

bool Recorder::prepare(const std::string &resolution_key)
{
    auto it = resolutions.find(resolution_key);
    if (it == resolutions.end())
    {
        std::cerr << "错误: 无效的分辨率 '" << resolution_key << "'." << std::endl;
        return false;
    }
    m_out_w = it->second.first;
    m_out_h = it->second.second;
    m_out_filename = std::string(TEMP_STORAGE_PATH) + generate_timestamp_filename();
    return true;
}

bool Recorder::isRecording() const { return m_is_recording; }

bool Recorder::initialize_ffmpeg()
{
    fprintf(stderr, "[录制器] 开始录制 到 %s (%dx%d)\n", m_out_filename.c_str(), m_out_w, m_out_h);
    int ret = 0;

    AVBufferRef* hw_device_ctx = m_capture_module->get_hw_device_context();
    if (hw_device_ctx != nullptr) {
        fprintf(stderr, "[录制器] 从采集器获取 RKMPP 硬件设备成功。\n");
        m_use_hw = true;
    } else {
        fprintf(stderr, "[录制器] 警告: 未获取到 RKMPP 硬件设备, 将回退到纯软件模式。\n");
        m_use_hw = false;
    }

    const AVCodec *enc = avcodec_find_encoder_by_name(RECORDER_ENCODER_NAME);
    if (!enc) {
        fprintf(stderr, "[录制器] 找不到编码器: %s\n", RECORDER_ENCODER_NAME);
        return false;
    }
    m_enc_ctx = avcodec_alloc_context3(enc);
    if (!m_enc_ctx) {
        fprintf(stderr, "[录制器] avcodec_alloc_context3 (enc) 失败\n");
        return false;
    }
    m_enc_ctx->width = m_out_w;
    m_enc_ctx->height = m_out_h;
    m_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    m_enc_ctx->time_base = AVRational{1, 1000000};
    m_enc_ctx->framerate = AVRational{30, 1};
    m_enc_ctx->bit_rate = (m_out_w * m_out_h > 1280 * 720) ? RECORDER_BITRATE_HIGH : RECORDER_BITRATE_LOW;
    m_enc_ctx->gop_size = RECORDER_GOP_SIZE;

    if (m_use_hw && hw_device_ctx) {
        m_enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }
    if ((ret = avcodec_open2(m_enc_ctx, enc, nullptr)) < 0) {
        print_err(ret, "avcodec_open2 (encoder)");
        return false;
    }

    avformat_alloc_output_context2(&m_ofmt_ctx, nullptr, nullptr, m_out_filename.c_str());
    if (!m_ofmt_ctx) {
        print_err(ret, "avformat_alloc_output_context2");
        return false;
    }
    if (m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        m_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    m_out_stream = avformat_new_stream(m_ofmt_ctx, nullptr);
    if (!m_out_stream) {
        fprintf(stderr, "[录制器] 创建输出流失败\n");
        return false;
    }
    avcodec_parameters_from_context(m_out_stream->codecpar, m_enc_ctx);
    m_out_stream->time_base = AVRational{1, 90000};

    if (!(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&m_ofmt_ctx->pb, m_out_filename.c_str(), AVIO_FLAG_WRITE)) < 0) {
            print_err(ret, "avio_open");
            return false;
        }
    }
    if ((ret = avformat_write_header(m_ofmt_ctx, nullptr)) < 0) {
        print_err(ret, "avformat_write_header");
        return false;
    }

    return true;
}

bool Recorder::reconfigure_filters()
{
    avfilter_graph_free(&m_filter_graph);
    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph) return false;

    AVCodecContext* dec_ctx = m_capture_module->get_decoder_context();
    AVBufferRef* hw_device_ctx = m_capture_module->get_hw_device_context();
    if (!dec_ctx) {
        fprintf(stderr, "[录制器] 错误: 无法从采集器获取解码器上下文。\n");
        return false;
    }

    const AVPixelFormat input_pix_fmt = dec_ctx->pix_fmt;
    const bool is_input_hw = (input_pix_fmt == AV_PIX_FMT_DRM_PRIME);

    int cx, cy, cw, ch;
    m_zoom_manager->get_crop_params(cx, cy, cw, ch);

    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char args[512];
    
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
             dec_ctx->width, dec_ctx->height, input_pix_fmt, 1, 1000000);
             
    if (is_input_hw && dec_ctx->hw_frames_ctx) {
        char hw_frames_ctx_arg[64];
        snprintf(hw_frames_ctx_arg, sizeof(hw_frames_ctx_arg), ":hw_frames_ctx=%p", (void*)dec_ctx->hw_frames_ctx);
        strcat(args, hw_frames_ctx_arg);
    }

    int ret = avfilter_graph_create_filter(&m_buffersrc_ctx, buffersrc, "in", args, nullptr, m_filter_graph);
    if (ret < 0) {
        print_err(ret, "avfilter_graph_create_filter (buffersrc)");
        return false;
    }

    ret = avfilter_graph_create_filter(&m_buffersink_ctx, buffersink, "out", nullptr, nullptr, m_filter_graph);
    if (ret < 0) {
        print_err(ret, "avfilter_graph_create_filter (buffersink)");
        return false;
    }
    enum AVPixelFormat sink_fmts[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
    av_opt_set_int_list(m_buffersink_ctx, "pix_fmts", sink_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    char filt_descr[1024];
    if (m_use_hw) {
        char rga_part[256];
        snprintf(rga_part, sizeof(rga_part), "vpp_rkrga=cx=%d:cy=%d:cw=%d:ch=%d:w=%d:h=%d",
                 cx, cy, cw, ch, m_out_w, m_out_h);
        
        if (is_input_hw) {
            fprintf(stderr, "[录制器] 检测到硬件帧输入(DRM_PRIME)，配置零拷贝滤镜路径。\n");
            snprintf(filt_descr, sizeof(filt_descr), "%s,hwdownload,format=nv12", rga_part);
        } else {
            fprintf(stderr, "[录制器] 检测到软件帧输入，配置 'hwupload' 滤镜路径。\n");
            snprintf(filt_descr, sizeof(filt_descr), "hwupload,%s,hwdownload,format=nv12", rga_part);
        }
    } else {
        snprintf(filt_descr, sizeof(filt_descr), "crop=%d:%d:%d:%d,scale=%d:%d,format=nv12",
                 cw, ch, cx, cy, m_out_w, m_out_h);
    }

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_buffersrc_ctx;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_buffersink_ctx;

    ret = avfilter_graph_parse_ptr(m_filter_graph, filt_descr, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        print_err(ret, "avfilter_graph_parse_ptr");
        return false;
    }

    if (m_use_hw && hw_device_ctx) {
        for (unsigned i = 0; i < m_filter_graph->nb_filters; i++) {
            AVFilterContext* fctx = m_filter_graph->filters[i];
            const char* filter_name = fctx->filter->name;
            if (strcmp(filter_name, "hwupload") == 0 || strcmp(filter_name, "vpp_rkrga") == 0) {
                fctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                fprintf(stderr, "[录制器] 已绑定 hw_device_ctx 到 %s 滤镜\n", filter_name);
            }
        }
    }

    if ((ret = avfilter_graph_config(m_filter_graph, nullptr)) < 0) {
        print_err(ret, "avfilter_graph_config");
        return false;
    }
    fprintf(stderr, "[录制器] 滤镜图配置完成: \"%s\"\n", filt_descr);
    return true;
}

void Recorder::run()
{
    m_is_recording = true;
    m_pipeline_error = false;
    m_stop_flag = false;
    
    if (!initialize_ffmpeg()) {
        fprintf(stderr, "[录制器] 错误: initialize_ffmpeg 失败\n");
        cleanup_ffmpeg();
        m_is_recording = false;
        return;
    }

    // 首次配置滤镜图时加锁
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        if (!reconfigure_filters()) {
            fprintf(stderr, "[录制器] 错误: 首次配置滤镜图失败\n");
            cleanup_ffmpeg();
            m_is_recording = false;
            return;
        }
    }
    
    m_capture_module->register_consumer(&m_queue_decoded_frames);

    fprintf(stderr, "[录制器] 启动流水线线程...\n");
    try {
        m_thread_filter = std::thread(&Recorder::thread_filter_osd, this);
        m_thread_encode = std::thread(&Recorder::thread_encode_write, this);
    } catch (const std::exception& e) {
        fprintf(stderr, "[录制器] 启动线程失败: %s\n", e.what());
        m_pipeline_error = true;
        stop();
    }
    
    if (m_thread_filter.joinable()) m_thread_filter.join();
    if (m_thread_encode.joinable()) m_thread_encode.join();

    fprintf(stderr, "[录制器] 流水线线程已全部退出。\n");
    
    m_capture_module->unregister_consumer(&m_queue_decoded_frames);

    cleanup_ffmpeg();
    
    if (!m_pipeline_error && m_stop_flag) {
        fprintf(stderr, "[录制器] 录制结束 保存: %s\n", m_out_filename.c_str());
        if (m_on_complete_cb) {
            m_on_complete_cb(m_out_filename);
        }
    } else {
        fprintf(stderr, "[录制器] 录制被中断 (错误或变焦)，删除临时文件: %s\n", m_out_filename.c_str());
        // unlink(m_out_filename.c_str());
    }

    m_is_recording = false;
}

void Recorder::stop()
{
    fprintf(stderr, "[录制器] 收到停止信号...\n");
    m_stop_flag = true;
    
    m_queue_decoded_frames.stop();
    m_queue_filtered_frames.stop();
}

void Recorder::cleanup_ffmpeg()
{
    fprintf(stderr, "[录制器] 正在清理 FFmpeg 资源...\n");

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
        m_filter_graph = nullptr; // 防止悬空指针
    }

    m_queue_decoded_frames.clear();
    m_queue_filtered_frames.clear();

    m_ofmt_ctx = nullptr;
    m_enc_ctx = nullptr;
    m_out_stream = nullptr;
}

void Recorder::thread_filter_osd()
{
    fprintf(stderr, "[T1:Filter] 滤镜OSD线程启动。\n");
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
            fprintf(stderr, "[T1:Filter] 检测到变焦，正在动态重建滤镜图...\n");
            
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            if (!reconfigure_filters()) {
                fprintf(stderr, "[T1:Filter] 错误: 动态重建滤镜失败，正在停止录制。\n");
                m_pipeline_error = true;
                break;
            }
            fprintf(stderr, "[T1:Filter] 滤镜图已成功更新。\n");
        }
        
        {
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            if (m_pipeline_error || !m_buffersrc_ctx) {
                continue;
            }
            // 注意：我们将已经校正过 PTS 的 frame 送入滤镜
            if (av_buffersrc_add_frame_flags(m_buffersrc_ctx, frame, 0) < 0) {
                fprintf(stderr, "[T1:Filter] 错误: av_buffersrc_add_frame 失败\n");
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
                print_err(ret, "av_buffersink_get_frame");
                m_pipeline_error = true;
                break;
            }

            if (m_osd_manager) {
                m_osd_manager->blend_osd_on_frame(filt_frame);
            }

            AVFrame* filt_frame_copy = av_frame_clone(filt_frame);
            if (!filt_frame_copy) {
                 fprintf(stderr, "[T1:Filter] 错误: av_frame_clone (filt) 失败\n");
                 m_pipeline_error = true;
                 break;
            }

            m_queue_filtered_frames.push(make_avframe_ptr(filt_frame_copy));
            av_frame_unref(filt_frame);
        }
    }

    av_frame_free(&filt_frame);
    m_queue_filtered_frames.stop();
    fprintf(stderr, "[T1:Filter] 滤镜OSD线程退出。\n");
}

void Recorder::thread_encode_write()
{
    fprintf(stderr, "[T2:Encode] 编码写入线程启动。\n");
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
            print_err(ret, "avcodec_send_frame (encoder)");
            m_pipeline_error = true;
            break;
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(m_enc_ctx, outpkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                print_err(ret, "avcodec_receive_packet (encoder)");
                m_pipeline_error = true;
                break;
            }

            av_packet_rescale_ts(outpkt, m_enc_ctx->time_base, m_out_stream->time_base);
            outpkt->stream_index = m_out_stream->index;
            
            ret = av_interleaved_write_frame(m_ofmt_ctx, outpkt);
            av_packet_unref(outpkt);
            if (ret < 0) {
                print_err(ret, "av_interleaved_write_frame");
                m_pipeline_error = true;
                break;
            }
        }
    }

    av_packet_free(&outpkt);
    m_queue_filtered_frames.stop();
    fprintf(stderr, "[T2:Encode] 编码写入线程退出。\n");
}