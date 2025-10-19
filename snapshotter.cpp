#include "snapshotter.h"
#include "app_config.h"
#include "osd_manager.h"
#include "zoom_manager.h"

#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

// 辅助函数：根据时间戳生成 .jpg 文件名
static std::string generate_jpg_timestamp_filename()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d%H%M%S");
    return ss.str() + ".jpg";
}

Snapshotter::Snapshotter(std::string device,
                         std::shared_ptr<OsdManager> osd_manager,
                         std::shared_ptr<ZoomManager> zoom_manager,
                         MediaCompleteCallback cb)
    : m_device_name(std::move(device)),
      m_osd_manager(std::move(osd_manager)),
      m_zoom_manager(std::move(zoom_manager)),
      m_on_complete_cb(std::move(cb)) {}

void Snapshotter::shoot()
{
    std::cout << "进入shoot函数" << std::endl;
    std::thread(&Snapshotter::run, this).detach();
}

void Snapshotter::run()
{
    // --- 资源声明 ---
    AVFormatContext *ifmt_ctx = nullptr;
    AVCodecContext *dec_ctx = nullptr;
    AVFrame *raw_frame = nullptr;
    AVPacket *pkt = nullptr;
    const AVCodec *jpeg_codec = nullptr;
    AVCodecContext *jpeg_ctx = nullptr;
    AVFrame *final_jpeg_frame = nullptr;
    AVFrame *nv12_frame_for_osd = nullptr;
    SwsContext *sws_ctx_to_nv12 = nullptr;
    SwsContext *sws_ctx_to_jpeg = nullptr;
    AVPacket *out_pkt = nullptr;

    std::string final_filename = generate_jpg_timestamp_filename();
    std::string temp_filename = TEMP_STORAGE_PATH + final_filename;

    do
    {
        // 1. 打开设备并获取一帧原始数据
        AVDictionary *opts = nullptr;
        char video_size_str[32];
        snprintf(video_size_str, sizeof(video_size_str), "%dx%d", V4L2_INPUT_WIDTH, V4L2_INPUT_HEIGHT);
        av_dict_set(&opts, "input_format", "nv12", 0);
        av_dict_set(&opts, "video_size", video_size_str, 0);
        const AVInputFormat *iformat = av_find_input_format("v4l2");
        if (avformat_open_input(&ifmt_ctx, m_device_name.c_str(), iformat, &opts) < 0)
            break;
        av_dict_free(&opts);
        if (avformat_find_stream_info(ifmt_ctx, nullptr) < 0)
            break;
        int video_stream_index = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream_index < 0)
            break;
        pkt = av_packet_alloc();
        if (av_read_frame(ifmt_ctx, pkt) < 0)
            break;

        // 2. 将原始数据包装进 AVFrame
        raw_frame = av_frame_alloc();
        dec_ctx = avcodec_alloc_context3(nullptr);
        avcodec_parameters_to_context(dec_ctx, ifmt_ctx->streams[video_stream_index]->codecpar);
        av_image_fill_arrays(raw_frame->data, raw_frame->linesize, pkt->data, dec_ctx->pix_fmt, dec_ctx->width, dec_ctx->height, 1);
        raw_frame->width = dec_ctx->width;
        raw_frame->height = dec_ctx->height;
        raw_frame->format = dec_ctx->pix_fmt;

        // --- 变焦集成点 ---
        int cx, cy, cw, ch;
        m_zoom_manager->get_crop_params(cx, cy, cw, ch);

        // 3. 将 *裁剪后* 的区域缩放为目标分辨率的 NV12 格式
        nv12_frame_for_osd = av_frame_alloc();
        nv12_frame_for_osd->format = AV_PIX_FMT_NV12;
        nv12_frame_for_osd->width = JPEG_OUTPUT_WIDTH;
        nv12_frame_for_osd->height = JPEG_OUTPUT_HEIGHT;
        if (av_frame_get_buffer(nv12_frame_for_osd, 0) < 0)
            break;

        sws_ctx_to_nv12 = sws_getContext(
            cw, ch, (AVPixelFormat)raw_frame->format, // 源尺寸是裁剪后的尺寸
            JPEG_OUTPUT_WIDTH, JPEG_OUTPUT_HEIGHT, AV_PIX_FMT_NV12,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_to_nv12)
            break;

        // 准备指向裁剪区域起始位置的指针
        uint8_t *src_data[4] = {nullptr};
        int src_stride[4] = {0};
        src_data[0] = raw_frame->data[0] + (cy * raw_frame->linesize[0]) + cx;
        src_data[1] = raw_frame->data[1] + ((cy / 2) * raw_frame->linesize[1]) + cx;
        src_stride[0] = raw_frame->linesize[0];
        src_stride[1] = raw_frame->linesize[1];

        // sws_scale 现在只处理裁剪后的区域
        sws_scale(sws_ctx_to_nv12,
                  (const uint8_t *const *)src_data, src_stride,
                  0, ch, // 从裁剪区域的y=0开始，处理ch行
                  nv12_frame_for_osd->data, nv12_frame_for_osd->linesize);

        if (m_osd_manager)
        {
            m_osd_manager->blend_osd_on_frame(nv12_frame_for_osd);
        }

        // 4. 准备JPEG编码
        jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!jpeg_codec)
            break;
        jpeg_ctx = avcodec_alloc_context3(jpeg_codec);
        jpeg_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        jpeg_ctx->width = JPEG_OUTPUT_WIDTH;
        jpeg_ctx->height = JPEG_OUTPUT_HEIGHT;
        jpeg_ctx->time_base = {1, 25};
        if (avcodec_open2(jpeg_ctx, jpeg_codec, nullptr) < 0)
            break;

        // 5. 将叠加了OSD的NV12帧转换为JPEG所需的YUVJ420P格式
        final_jpeg_frame = av_frame_alloc();
        final_jpeg_frame->format = jpeg_ctx->pix_fmt;
        final_jpeg_frame->width = jpeg_ctx->width;
        final_jpeg_frame->height = jpeg_ctx->height;
        if (av_frame_get_buffer(final_jpeg_frame, 0) < 0)
            break;

        sws_ctx_to_jpeg = sws_getContext(JPEG_OUTPUT_WIDTH, JPEG_OUTPUT_HEIGHT, AV_PIX_FMT_NV12,
                                         jpeg_ctx->width, jpeg_ctx->height, jpeg_ctx->pix_fmt,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_to_jpeg)
            break;
        sws_scale(sws_ctx_to_jpeg, nv12_frame_for_osd->data, nv12_frame_for_osd->linesize, 0, JPEG_OUTPUT_HEIGHT, final_jpeg_frame->data, final_jpeg_frame->linesize);

        // 6. 编码并写入文件
        out_pkt = av_packet_alloc();
        if (avcodec_send_frame(jpeg_ctx, final_jpeg_frame) == 0)
        {
            if (avcodec_receive_packet(jpeg_ctx, out_pkt) == 0)
            {
                FILE *f = fopen(temp_filename.c_str(), "wb");
                if (f)
                {
                    fwrite(out_pkt->data, 1, out_pkt->size, f);
                    fclose(f);
                    printf("[拍照器] 成功保存快照至: %s\n", temp_filename.c_str());
                    if (m_on_complete_cb)
                        m_on_complete_cb(temp_filename);
                }
                else
                {
                    std::cerr << "[拍照器] 错误: fopen 失败: " << strerror(errno) << std::endl;
                }
            }
        }
    } while (false);

    // 7. 清理所有资源
    av_packet_free(&out_pkt);
    sws_freeContext(sws_ctx_to_nv12);
    sws_freeContext(sws_ctx_to_jpeg);
    av_frame_free(&final_jpeg_frame);
    av_frame_free(&nv12_frame_for_osd);
    avcodec_free_context(&jpeg_ctx);
    av_packet_free(&pkt);
    av_frame_free(&raw_frame);
    avcodec_free_context(&dec_ctx);
    if (ifmt_ctx)
        avformat_close_input(&ifmt_ctx);

    delete this;
}
