// --- START OF FILE snapshotter.cpp ---

#include "snapshotter.h"
#include "app_config.h"
#include "osd_manager.h"
#include "zoom_manager.h"
#include "camera_capture.h"

#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

static std::string generate_jpg_timestamp_filename()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d%H%M%S");
    return ss.str() + ".jpg";
}

static void print_err_snap(int ret, const char* context) {
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "[拍照器] FFmpeg 错误 in %s: %s (ret=%d)\n", context, buf, ret);
}

Snapshotter::Snapshotter(CameraCapture* capture_module,
                         std::shared_ptr<OsdManager> osd_manager,
                         std::shared_ptr<ZoomManager> zoom_manager,
                         MediaCompleteCallback cb)
    : m_capture_module(capture_module),
      m_osd_manager(std::move(osd_manager)),
      m_zoom_manager(std::move(zoom_manager)),
      m_on_complete_cb(std::move(cb)) {}

Snapshotter::~Snapshotter() {
    cleanup_filter_graph();
}

void Snapshotter::cleanup_filter_graph() {
    avfilter_graph_free(&m_filter_graph);
    m_filter_graph = nullptr;
    m_buffersrc_ctx = nullptr;
    m_buffersink_ctx = nullptr;
}

bool Snapshotter::setup_filter_graph(AVFrame* in_frame) {
    cleanup_filter_graph();

    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph) return false;

    int cx, cy, cw, ch;
    m_zoom_manager->get_crop_params(cx, cy, cw, ch);

    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char args[512];
    
    AVCodecContext* dec_ctx = m_capture_module->get_decoder_context();
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/%d",
             in_frame->width, in_frame->height, in_frame->format, 
             1, 1000000, 
             dec_ctx->framerate.num, dec_ctx->framerate.den);

    int ret = avfilter_graph_create_filter(&m_buffersrc_ctx, buffersrc, "in", args, nullptr, m_filter_graph);
    if (ret < 0) { print_err_snap(ret, "create buffersrc"); return false; }
    
    ret = avfilter_graph_create_filter(&m_buffersink_ctx, buffersink, "out", nullptr, nullptr, m_filter_graph);
    if (ret < 0) { print_err_snap(ret, "create buffersink"); return false; }
    
    enum AVPixelFormat sink_fmts[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
    av_opt_set_int_list(m_buffersink_ctx, "pix_fmts", sink_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    char filt_descr[512];
    bool use_hw = (m_capture_module->get_hw_device_context() != nullptr);
    if (use_hw) {
        snprintf(filt_descr, sizeof(filt_descr), "hwupload,vpp_rkrga=cx=%d:cy=%d:cw=%d:ch=%d:w=%d:h=%d,hwdownload,format=nv12",
                 cx, cy, cw, ch, JPEG_OUTPUT_WIDTH, JPEG_OUTPUT_HEIGHT);
    } else {
        snprintf(filt_descr, sizeof(filt_descr), "crop=%d:%d:%d:%d,scale=%d:%d,format=nv12",
                 cw, ch, cx, cy, JPEG_OUTPUT_WIDTH, JPEG_OUTPUT_HEIGHT);
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
    if (ret < 0) { print_err_snap(ret, "graph_parse_ptr"); return false; }

    if (use_hw) {
        AVBufferRef* hw_device_ctx = m_capture_module->get_hw_device_context();
        if (hw_device_ctx) {
            for (unsigned i = 0; i < m_filter_graph->nb_filters; i++) {
                AVFilterContext* fctx = m_filter_graph->filters[i];
                if (!fctx || !fctx->filter) continue;
                const char* filter_name = fctx->filter->name;
                if (strcmp(filter_name, "hwupload") == 0 || strcmp(filter_name, "vpp_rkrga") == 0) {
                    fctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                }
            }
        }
    }


    if (avfilter_graph_config(m_filter_graph, nullptr) < 0) {
        print_err_snap(ret, "graph_config");
        return false;
    }
    
    fprintf(stderr, "[拍照器] 成功创建滤镜图: \"%s\"\n", filt_descr);
    return true;
}

void Snapshotter::run()
{
    AVFramePtr raw_frame_ptr = nullptr;
    AVFrame *processed_frame = nullptr;
    AVFrame *final_jpeg_frame = nullptr;
    const AVCodec *jpeg_codec = nullptr;
    AVCodecContext *jpeg_ctx = nullptr;
    SwsContext *sws_ctx_to_jpeg = nullptr;
    AVPacket *out_pkt = nullptr;
    
    std::string temp_filename = TEMP_STORAGE_PATH + generate_jpg_timestamp_filename();
    bool success = false;
    
    do
    {
        if (!m_capture_module) {
             std::cerr << "[拍照器] 错误: 采集模块无效。" << std::endl;
             break;
        }

        fprintf(stderr, "[拍照器] 正在向采集器请求一帧...\n");
        std::future<AVFramePtr> frame_future = m_capture_module->request_single_frame();
        
        if (frame_future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            std::cerr << "[拍照器] 错误: 等待帧超时。" << std::endl;
            break;
        }

        raw_frame_ptr = frame_future.get();

        if (raw_frame_ptr == nullptr) {
            std::cerr << "[拍照器] 错误: 未能从采集器获取到有效帧。" << std::endl;
            break;
        }
        fprintf(stderr, "[拍照器] 成功获取一帧。\n");
        
        AVFrame* frame = raw_frame_ptr.get();
        frame->pts = 0;

        if (!setup_filter_graph(frame)) {
            fprintf(stderr, "[拍照器] 错误: 创建滤镜图失败。\n");
            break;
        }

        if (av_buffersrc_add_frame(m_buffersrc_ctx, frame) < 0) {
            fprintf(stderr, "[拍照器] 错误: av_buffersrc_add_frame 失败。\n");
            break;
        }

        if (av_buffersrc_add_frame(m_buffersrc_ctx, NULL) < 0) {
            fprintf(stderr, "[拍照器] 错误: 冲刷滤镜图失败 (发送NULL帧)。\n");
            break;
        }

        processed_frame = av_frame_alloc();
        if (!processed_frame) break;

        bool got_frame = false;
        while (true) {
            int ret = av_buffersink_get_frame(m_buffersink_ctx, processed_frame);
            if (ret == AVERROR(EAGAIN)) {
                continue;
            } else if (ret == AVERROR_EOF) {
                if (!got_frame) {
                    fprintf(stderr, "[拍照器] 滤镜图已冲刷完毕，但未获取到任何帧。\n");
                }
                break;
            } else if (ret < 0) {
                print_err_snap(ret, "av_buffersink_get_frame");
                break;
            }
            
            fprintf(stderr, "[拍照器] 成功通过滤镜处理帧。\n");
            got_frame = true;
            break;
        }

        if (!got_frame) {
            break;
        }

        if (m_osd_manager) {
            m_osd_manager->blend_osd_on_frame(processed_frame);
        }

        jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!jpeg_codec) {
             std::cerr << "[拍照器] 错误: avcodec_find_encoder (MJPEG) 失败" << std::endl;
            break;
        }
            
        jpeg_ctx = avcodec_alloc_context3(jpeg_codec);
        jpeg_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        jpeg_ctx->width = JPEG_OUTPUT_WIDTH;
        jpeg_ctx->height = JPEG_OUTPUT_HEIGHT;
        jpeg_ctx->time_base = {1, 25};
        jpeg_ctx->framerate = {25, 1};
        if (avcodec_open2(jpeg_ctx, jpeg_codec, nullptr) < 0) {
             std::cerr << "[拍照器] 错误: avcodec_open2 (jpeg) 失败" << std::endl;
            break;
        }
            
        final_jpeg_frame = av_frame_alloc();
        final_jpeg_frame->format = jpeg_ctx->pix_fmt;
        final_jpeg_frame->width = jpeg_ctx->width;
        final_jpeg_frame->height = jpeg_ctx->height;
        if (av_frame_get_buffer(final_jpeg_frame, 0) < 0) {
             std::cerr << "[拍照器] 错误: av_frame_get_buffer (jpeg) 失败" << std::endl;
            break;
        }
            
        sws_ctx_to_jpeg = sws_getContext(JPEG_OUTPUT_WIDTH, JPEG_OUTPUT_HEIGHT, AV_PIX_FMT_NV12,
                                         jpeg_ctx->width, jpeg_ctx->height, jpeg_ctx->pix_fmt,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_to_jpeg) {
             std::cerr << "[拍照器] 错误: sws_getContext (to_jpeg) 失败" << std::endl;
            break;
        }
            
        sws_scale(sws_ctx_to_jpeg, processed_frame->data, processed_frame->linesize, 0, JPEG_OUTPUT_HEIGHT, final_jpeg_frame->data, final_jpeg_frame->linesize);

        out_pkt = av_packet_alloc();
        if (avcodec_send_frame(jpeg_ctx, final_jpeg_frame) == 0) {
            if (avcodec_receive_packet(jpeg_ctx, out_pkt) == 0) {
                FILE *f = fopen(temp_filename.c_str(), "wb");
                if (f) {
                    fwrite(out_pkt->data, 1, out_pkt->size, f);
                    fclose(f);
                    printf("[拍照器] 成功保存快照至: %s\n", temp_filename.c_str());
                    if (m_on_complete_cb) m_on_complete_cb(temp_filename);
                    success = true;
                } else {
                    std::cerr << "[拍照器] 错误: fopen 失败: " << strerror(errno) << std::endl;
                }
            } else {
                 std::cerr << "[拍照器] 错误: avcodec_receive_packet 失败" << std::endl;
            }
        } else {
            std::cerr << "[拍照器] 错误: avcodec_send_frame 失败" << std::endl;
        }
    } while (false);

    cleanup_filter_graph();
    av_packet_free(&out_pkt);
    sws_freeContext(sws_ctx_to_jpeg);
    av_frame_free(&final_jpeg_frame);
    av_frame_free(&processed_frame);
    avcodec_free_context(&jpeg_ctx);
    
    if (!success) {
        fprintf(stderr, "[拍照器] 拍照任务失败，未保存文件。\n");
    }
}