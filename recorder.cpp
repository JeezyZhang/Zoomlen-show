#include "recorder.h"
#include "app_config.h"
#include "osd_manager.h"
#include "zoom_manager.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavdevice/avdevice.h>
}

// 辅助函数：根据时间戳生成 .mp4 文件名
static std::string generate_timestamp_filename()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d%H%M%S");
    return ss.str() + ".mp4";
}

// 辅助函数：打印 FFmpeg 错误
static void print_err(int ret, const char *context)
{
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "[录制器] FFmpeg 错误 in %s: %s\n", context, buf);
}

// 支持的分辨率映射
const std::map<std::string, std::pair<int, int>> resolutions = {
    {"1080p", {1920, 1080}},
    {"720p", {1280, 720}},
    {"360p", {640, 360}}
};

Recorder::Recorder(std::string device,
                   std::shared_ptr<OsdManager> osd_manager,
                   std::shared_ptr<ZoomManager> zoom_manager,
                   MediaCompleteCallback cb)
    : m_device_name(std::move(device)),
      m_osd_manager(std::move(osd_manager)),
      m_zoom_manager(std::move(zoom_manager)),
      m_on_complete_cb(std::move(cb)) {}

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
    std::string basename = generate_timestamp_filename();
    m_out_filename = TEMP_STORAGE_PATH + basename;
    m_stop_flag = false;
    return true;
}

void Recorder::stop() { m_stop_flag = true; }
bool Recorder::isRecording() const { return m_is_recording; }

// 新增: 滤镜图重建函数
bool Recorder::reconfigure_filters()
{
    // 销毁旧的滤镜图（如果存在）
    avfilter_graph_free(&m_filter_graph);

    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph)
        return false;

    // 获取最新的变焦裁剪参数
    int cx, cy, cw, ch;
    m_zoom_manager->get_crop_params(cx, cy, cw, ch);

    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d", m_dec_ctx->width, m_dec_ctx->height, m_dec_ctx->pix_fmt, 1, 1000000);
    avfilter_graph_create_filter(&m_buffersrc_ctx, buffersrc, "in", args, nullptr, m_filter_graph);
    avfilter_graph_create_filter(&m_buffersink_ctx, buffersink, "out", nullptr, nullptr, m_filter_graph);
    enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
    av_opt_set_int_list(m_buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    char filt_descr[512];
    snprintf(filt_descr, sizeof(filt_descr),
             "hwupload,"
             "vpp_rkrga=cx=%d:cy=%d:cw=%d:ch=%d:w=%d:h=%d,"
             "hwdownload,format=nv12",
             cx, cy, cw, ch, m_out_w, m_out_h);

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_buffersrc_ctx;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_buffersink_ctx;
    int ret = avfilter_graph_parse_ptr(m_filter_graph, filt_descr, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0)
    {
        print_err(ret, "avfilter_graph_parse_ptr");
        return false;
    }

    if (m_hw_device_ctx)
    {
        for (unsigned i = 0; i < m_filter_graph->nb_filters; ++i)
        {
            if (!strcmp(m_filter_graph->filters[i]->filter->name, "hwupload"))
                m_filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
        }
    }

    if ((ret = avfilter_graph_config(m_filter_graph, nullptr)) < 0)
    {
        print_err(ret, "avfilter_graph_config");
        return false;
    }

    printf("[录制器] 滤镜图已更新，变焦参数已应用\n");
    return true;
}

void Recorder::run()
{
    m_is_recording = true;
    printf("[录制器] 正在录制到临时文件: %s (%dx%d)\n", m_out_filename.c_str(), m_out_w, m_out_h);

    int video_stream_index = -1;
    int ret;

    // --- 1. 打开 V4L2 和解码器上下文 (仅一次) ---
    AVDictionary *opts = nullptr;
    char video_size_str[32];
    snprintf(video_size_str, sizeof(video_size_str), "%dx%d", V4L2_INPUT_WIDTH, V4L2_INPUT_HEIGHT);
    av_dict_set(&opts, "input_format", "nv12", 0);
    av_dict_set(&opts, "framerate", "30", 0);
    av_dict_set(&opts, "video_size", video_size_str, 0);
    const AVInputFormat *iformat = av_find_input_format("v4l2");
    if ((ret = avformat_open_input(&m_ifmt_ctx, m_device_name.c_str(), iformat, &opts)) < 0)
    {
        print_err(ret, "avformat_open_input");
        cleanup();
        return;
    }
    av_dict_free(&opts);
    if ((ret = avformat_find_stream_info(m_ifmt_ctx, nullptr)) < 0)
    {
        print_err(ret, "avformat_find_stream_info");
        cleanup();
        return;
    }
    video_stream_index = av_find_best_stream(m_ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0)
    {
        fprintf(stderr, "找不到视频流\n");
        cleanup();
        return;
    }
    AVStream *in_video_stream = m_ifmt_ctx->streams[video_stream_index];
    const AVCodec *decoder = avcodec_find_decoder(in_video_stream->codecpar->codec_id);
    m_dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(m_dec_ctx, in_video_stream->codecpar);
    if ((ret = avcodec_open2(m_dec_ctx, decoder, nullptr)) < 0)
    {
        print_err(ret, "avcodec_open2 for decoder");
        cleanup();
        return;
    }

    // --- 2. 初始化硬件、编码器和封装器 (仅一次) ---
    av_hwdevice_ctx_create(&m_hw_device_ctx, AV_HWDEVICE_TYPE_RKMPP, "rkmpp", nullptr, 0);
    const AVCodec *enc = avcodec_find_encoder_by_name(VIDEO_ENCODER_NAME);
    m_enc_ctx = avcodec_alloc_context3(enc);
    m_enc_ctx->width = m_out_w;
    m_enc_ctx->height = m_out_h;
    m_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    m_enc_ctx->time_base = {1, 1000000};
    m_enc_ctx->framerate = {30, 1};
    m_enc_ctx->bit_rate = (m_out_w * m_out_h > 1280 * 720) ? 8000000 : 4000000;
    m_enc_ctx->gop_size = 50;
    if ((ret = avcodec_open2(m_enc_ctx, enc, nullptr)) < 0)
    {
        print_err(ret, "avcodec_open2 for encoder");
        cleanup();
        return;
    }
    avformat_alloc_output_context2(&m_ofmt_ctx, nullptr, nullptr, m_out_filename.c_str());
    if (m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        m_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVStream *out_stream = avformat_new_stream(m_ofmt_ctx, nullptr);
    avcodec_parameters_from_context(out_stream->codecpar, m_enc_ctx);
    out_stream->time_base = {1, 90000};
    if (!(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE))
    {
        if ((ret = avio_open(&m_ofmt_ctx->pb, m_out_filename.c_str(), AVIO_FLAG_WRITE)) < 0)
        {
            print_err(ret, "avio_open");
            cleanup();
            return;
        }
    }
    if ((ret = avformat_write_header(m_ofmt_ctx, nullptr)) < 0)
    {
        print_err(ret, "avformat_write_header");
        cleanup();
        return;
    }

    // --- 3. 首次配置滤镜图 ---
    if (!reconfigure_filters())
    {
        fprintf(stderr, "[录制器] 错误: 首次配置滤镜图失败\n");
        cleanup();
        return;
    }

    // --- 4. 主循环 ---
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket *outpkt = av_packet_alloc();
    int64_t first_pts = AV_NOPTS_VALUE;

    printf("[录制器] 录制主循环已启动...\n");
    while (!m_stop_flag)
    {

        // **变焦集成点**: 检查是否需要重建滤镜图
        if (m_zoom_manager && m_zoom_manager->check_and_reset_change_flag())
        {
            if (!reconfigure_filters())
            {
                fprintf(stderr, "[录制器] 错误: 更新变焦失败，将停止录制\n");
                break;
            }
        }

        if (av_read_frame(m_ifmt_ctx, pkt) < 0)
            break;
        if (pkt->stream_index != video_stream_index)
        {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(m_dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            continue;

        while (avcodec_receive_frame(m_dec_ctx, frame) == 0)
        {
            if (first_pts == AV_NOPTS_VALUE)
                first_pts = frame->pts;
            frame->pts -= first_pts;
            if (frame->pts < 0)
                frame->pts = 0;

            if (av_buffersrc_add_frame_flags(m_buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
            {
                av_frame_unref(frame);
                break;
            }
            av_frame_unref(frame);

            while (av_buffersink_get_frame(m_buffersink_ctx, filt_frame) == 0)
            {
                if (m_osd_manager)
                {
                    m_osd_manager->blend_osd_on_frame(filt_frame);
                }
                filt_frame->pts = av_rescale_q(filt_frame->pts, m_buffersink_ctx->inputs[0]->time_base, m_enc_ctx->time_base);

                if (avcodec_send_frame(m_enc_ctx, filt_frame) >= 0)
                {
                    while (avcodec_receive_packet(m_enc_ctx, outpkt) >= 0)
                    {
                        av_packet_rescale_ts(outpkt, m_enc_ctx->time_base, out_stream->time_base);
                        outpkt->stream_index = out_stream->index;
                        av_interleaved_write_frame(m_ofmt_ctx, outpkt);
                        av_packet_unref(outpkt);
                    }
                }
                av_frame_unref(filt_frame);
            }
        }
    }

    // --- 5. 清理 ---
    printf("[录制器] 正在冲刷编码器并写入文件尾...\n");
    if (avcodec_send_frame(m_enc_ctx, nullptr) >= 0)
    {
        while (avcodec_receive_packet(m_enc_ctx, outpkt) >= 0)
        {
            av_packet_rescale_ts(outpkt, m_enc_ctx->time_base, out_stream->time_base);
            outpkt->stream_index = out_stream->index;
            av_interleaved_write_frame(m_ofmt_ctx, outpkt);
            av_packet_unref(outpkt);
        }
    }
    av_write_trailer(m_ofmt_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&outpkt);

    printf("[录制器] 录制完成; 临时文件已保存至 %s\n", m_out_filename.c_str());
    if (m_on_complete_cb)
        m_on_complete_cb(m_out_filename);

    cleanup();
}

void Recorder::cleanup()
{
    if (m_ofmt_ctx && !(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m_ofmt_ctx->pb);
    avformat_free_context(m_ofmt_ctx);
    avcodec_free_context(&m_enc_ctx);
    avcodec_free_context(&m_dec_ctx);
    avfilter_graph_free(&m_filter_graph); // 确保滤镜图也被释放
    if (m_ifmt_ctx)
        avformat_close_input(&m_ifmt_ctx);
    if (m_hw_device_ctx)
        av_buffer_unref(&m_hw_device_ctx);
    m_ifmt_ctx = nullptr;
    m_ofmt_ctx = nullptr;
    m_dec_ctx = nullptr;
    m_enc_ctx = nullptr;
    m_filter_graph = nullptr;
    m_hw_device_ctx = nullptr;
    m_is_recording = false;
}
