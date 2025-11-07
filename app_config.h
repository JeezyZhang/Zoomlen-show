#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/**
 * @file app_config.h
 * @brief 应用全局配置头文件。
 *
 * 将所有可调整的“魔数”集中在此处，作为宏定义，方便统一修改和管理。
 */

// ======================================================================
// =                         文件系统路径配置                           =
// ======================================================================
// 视频和照片的临时存储路径 (建议使用 /tmp，通常是内存文件系统，速度快)
#define TEMP_STORAGE_PATH   "/tmp/"
// 视频和照片的最终存储路径 (例如 SD 卡挂载点)
#define FINAL_STORAGE_PATH  "/mnt/sdcard/"


// ======================================================================
// =                         摄像头与视频配置                           =
// ======================================================================
// V4L2 摄像头设备期望的原始输入分辨率
#define V4L2_INPUT_WIDTH    2112
#define V4L2_INPUT_HEIGHT   1568
// FFmpeg H.264 硬件编码器名称 (用于录制和推流)
#define H264_ENCODER_NAME "h264_rkmpp"


// ======================================================================
// =                         视频录制 (Recording) 配置                  =
// ======================================================================
// 录制视频文件时使用的编码器
#define RECORDER_ENCODER_NAME H264_ENCODER_NAME
// 录制高分辨率视频 (>=720p) 的比特率
#define RECORDER_BITRATE_HIGH 8000000 // 8 Mbps
// 录制低分辨率视频 (<720p) 的比特率
#define RECORDER_BITRATE_LOW  4000000 // 4 Mbps
// 录制视频的GOP (Group of Pictures) 大小
#define RECORDER_GOP_SIZE 50


// ======================================================================
// =                         RTSP 推流 (Streaming) 配置                 =
// ======================================================================
// RTSP推流时使用的编码器
#define RTSP_ENCODER_NAME   H264_ENCODER_NAME
// RTSP推流的目标分辨率
#define RTSP_OUTPUT_WIDTH   1920
#define RTSP_OUTPUT_HEIGHT  1080
// RTSP推流的比特率
#define RTSP_BITRATE        4000000 // 4 Mbps
// RTSP推流的GOP大小
#define RTSP_GOP_SIZE       30
// RTSP推流使用的传输协议 ("udp" 或 "tcp")
#define RTSP_TRANSPORT      "udp"


// ======================================================================
// =                         OSD (屏幕显示) 配置                        =
// ======================================================================
// OSD 图层的分辨率 (应与最高输出视频分辨率保持一致)
#define OSD_BUFFER_WIDTH    1920
#define OSD_BUFFER_HEIGHT   1080
// OSD 字体大小
#define OSD_FONT_SIZE       36
// OSD 字体文件的绝对路径 (请确保此路径在目标系统上有效)
#define OSD_FONT_PATH       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


// ======================================================================
// =                         拍照 (Snapshot) 配置                       =
// ======================================================================
// 拍照生成的 JPEG 图片的最终分辨率
#define JPEG_OUTPUT_WIDTH   1920
#define JPEG_OUTPUT_HEIGHT  1080

#endif // APP_CONFIG_H

