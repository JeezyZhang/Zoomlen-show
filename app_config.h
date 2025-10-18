#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// ======================================================================
// =                         文件系统路径配置                           =
// ======================================================================

// 视频和照片的临时存储路径 (内存文件系统，速度快)
#define TEMP_STORAGE_PATH   "/tmp/"

// 视频和照片的最终存储路径 (SD卡挂载点)
#define FINAL_STORAGE_PATH  "/mnt/sdcard/"

// ======================================================================
// =                         摄像头与视频配置                           =
// ======================================================================

// V4L2 摄像头设备的输入分辨率
#define V4L2_INPUT_WIDTH    2112
#define V4L2_INPUT_HEIGHT   1568

// FFmpeg HEVC 编码器名称
#define VIDEO_ENCODER_NAME  "hevc_rkmpp"

// ======================================================================
// =                         OSD (屏幕显示) 配置                        =
// ======================================================================

// OSD 图层的分辨率 (与最高输出视频分辨率保持一致)
#define OSD_BUFFER_WIDTH    1920
#define OSD_BUFFER_HEIGHT   1080

// OSD 字体大小
#define OSD_FONT_SIZE       36

// OSD 字体文件的绝对路径
#define OSD_FONT_PATH       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

// ======================================================================
// =                         拍照 (Snapshot) 配置                       =
// ======================================================================

// 拍照生成的 JPEG 图片的最终分辨率
#define JPEG_OUTPUT_WIDTH   1920
#define JPEG_OUTPUT_HEIGHT  1080

#endif // APP_CONFIG_H
