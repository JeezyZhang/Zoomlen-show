#ifndef OSD_MANAGER_H
#define OSD_MANAGER_H

#include <atomic>
#include <string>
#include <thread>
#include <mutex>

// 向前声明 FFmpeg 的 AVFrame 结构体，避免在此头文件中引入过多 FFmpeg 头文件
struct AVFrame;

// FreeType 头文件
#include <ft2build.h>
#include FT_FREETYPE_H

// RGA/im2d 头文件
#include <im2d.hpp>
#include <RockchipRga.h>
#include <RgaUtils.h>

/**
 * @class OsdManager
 * @brief 管理所有与OSD叠加相关的功能。
 *
 * - 初始化和清理 FreeType 和 RGA 资源。
 * - 接收外部传入的POS数据。
 * - 提供一个核心方法，将 OSD 图层（包括半透明背景）通过 RGA 硬件合成到视频帧上。
 */
class OsdManager {
public:
    // 定义POS数据结构体
    struct PosData {
        double latitude;
        double longitude;
        float speed_kmh;
        std::string timestamp;
    };

    OsdManager();
    ~OsdManager();

    // 初始化所有资源 (FreeType, RGA)
    bool initialize();
    
    // 安全关闭并释放所有资源
    void shutdown();

    // 开启或关闭 OSD 功能
    void enable(bool state);

    // 检查 OSD 功能是否已开启
    bool is_enabled() const;

    // 设置要显示的POS数据 (线程安全)
    void set_pos_data(const PosData& data);

    // 核心功能：将当前的 OSD 图层叠加到给定的视频帧上
    void blend_osd_on_frame(AVFrame *frame);

private:
    // FreeType 和 RGA 的初始化与清理函数
    bool init_freetype();
    void cleanup_freetype();
    bool init_rga();
    void cleanup_rga();

    // OSD 文本和背景绘制辅助函数
    void draw_text(const std::string& text, int x_start, int y_start);
    void clear_osd_buffer();
    void draw_background();
    
    // 成员变量
    std::atomic<bool> m_enabled{false};         // OSD 开关状态
    std::atomic<bool> m_shutdown_flag{false};   // 退出标志
    std::mutex m_data_mutex;                    // 保护共享数据的互斥锁

    PosData m_pos_data;                         // 要显示的POS数据

    // FreeType 相关资源
    FT_Library ft_library = nullptr;
    FT_Face    ft_face    = nullptr;

    // RGA 相关资源
    char* m_osd_buffer      = nullptr; // OSD 图层的 RGBA 缓冲区
    rga_buffer_t      m_rga_src_osd;               // OSD RGA 源
    rga_buffer_handle_t m_rga_src_handle  = -1;      // OSD RGA 句柄
};

#endif // OSD_MANAGER_H

