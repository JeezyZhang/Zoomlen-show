#include "osd_manager.h"
#include "app_config.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring> // For memset

// FFmpeg 头文件
extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h> // [优化] 包含像素格式定义
}

/**
 * @file osd_manager.cpp
 * @brief 实现了 OsdManager 类的功能。
 */

// [修复] 为兼容C++11，在构造函数体内逐个成员进行初始化
OsdManager::OsdManager()
{
    m_pos_data.latitude = 0.0;
    m_pos_data.longitude = 0.0;
    m_pos_data.speed_kmh = 0.0f;
    m_pos_data.timestamp = "----/--/-- --:--:--";
}

OsdManager::~OsdManager()
{
    // 确保在对象析构时，资源一定会被释放
    if (!m_shutdown_flag.load())
    {
        shutdown();
    }
}

bool OsdManager::initialize()
{
    if (!init_freetype())
        return false;
    if (!init_rga())
        return false;

    m_shutdown_flag = false;

    std::cout << "[OSD管理器] 初始化成功。" << std::endl;
    return true;
}

void OsdManager::shutdown()
{
    m_shutdown_flag = true;
    cleanup_rga();
    cleanup_freetype();
    std::cout << "[OSD管理器] 已关闭。" << std::endl;
}

void OsdManager::enable(bool state)
{
    m_enabled = state;
    std::cout << "[OSD管理器] OSD 功能已 " << (state ? "开启" : "关闭") << std::endl;
}

bool OsdManager::is_enabled() const
{
    return m_enabled;
}

void OsdManager::set_pos_data(const PosData &data)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (!m_shutdown_flag)
    {
        m_pos_data = data;
    }
}

bool OsdManager::init_freetype()
{
    if (FT_Init_FreeType(&ft_library))
    {
        std::cerr << "[OSD管理器] 错误: 初始化 FreeType 库失败\n";
        return false;
    }
    if (FT_New_Face(ft_library, OSD_FONT_PATH, 0, &ft_face))
    {
        std::cerr << "[OSD管理器] 错误: 加载字体失败: " << OSD_FONT_PATH << "\n";
        FT_Done_FreeType(ft_library);
        return false;
    }
    FT_Set_Pixel_Sizes(ft_face, 0, OSD_FONT_SIZE);
    return true;
}

void OsdManager::cleanup_freetype()
{
    if (ft_face)
        FT_Done_Face(ft_face);
    if (ft_library)
        FT_Done_FreeType(ft_library);
    ft_face = nullptr;
    ft_library = nullptr;
}

bool OsdManager::init_rga()
{
    int bpp = 4; // RGBA8888
    m_osd_buffer = (char *)malloc(OSD_BUFFER_WIDTH * OSD_BUFFER_HEIGHT * bpp);
    if (!m_osd_buffer)
    {
        std::cerr << "[OSD管理器] 错误: 分配 OSD 缓冲区内存失败。" << std::endl;
        return false;
    }
    // 初始时清空为全透明
    memset(m_osd_buffer, 0, OSD_BUFFER_WIDTH * OSD_BUFFER_HEIGHT * bpp);

    m_rga_src_handle = importbuffer_virtualaddr(m_osd_buffer, OSD_BUFFER_WIDTH, OSD_BUFFER_HEIGHT, RK_FORMAT_RGBA_8888);
    if (m_rga_src_handle <= 0)
    {
        std::cerr << "[OSD管理器] 错误: 导入 OSD 缓冲区到 RGA 失败!\n";
        free(m_osd_buffer);
        m_osd_buffer = nullptr;
        return false;
    }

    m_rga_src_osd = wrapbuffer_handle(m_rga_src_handle, OSD_BUFFER_WIDTH, OSD_BUFFER_HEIGHT, RK_FORMAT_RGBA_8888);
    if (m_rga_src_osd.width == 0)
    {
        fprintf(stderr, "[OSD管理器] 错误: 包装 RGA OSD 句柄失败: %s\n", imStrError());
        releasebuffer_handle(m_rga_src_handle);
        free(m_osd_buffer);
        m_osd_buffer = nullptr;
        return false;
    }
    return true;
}

void OsdManager::cleanup_rga()
{
    if (m_rga_src_handle > 0)
    {
        releasebuffer_handle(m_rga_src_handle);
        m_rga_src_handle = -1;
    }
    if (m_osd_buffer)
    {
        free(m_osd_buffer);
        m_osd_buffer = nullptr;
    }
}

void OsdManager::clear_osd_buffer()
{
    // 定义一个足够大的“脏矩形”区域来覆盖两行文字及其背景
    const int osd_x = 40, osd_y = 30; // 调整Y坐标和高度以增加边距
    const int osd_w = 900, osd_h = OSD_FONT_SIZE * 2 + 40;

    // 仅清除这个脏矩形区域为全透明(0)
    for (int row = osd_y; row < osd_y + osd_h; ++row)
    {
        if (row >= OSD_BUFFER_HEIGHT)
            break; // 边界检查
        memset(m_osd_buffer + (row * OSD_BUFFER_WIDTH + osd_x) * 4, 0, osd_w * 4);
    }
}

void OsdManager::draw_background()
{
    const int osd_x = 40, osd_y = 30;
    const int osd_w = 900, osd_h = OSD_FONT_SIZE * 2 + 40;
    const uint32_t bg_color = 0x80000000; // RGBA8888, 格式为 0xAARRGGBB，这里是半透明黑色

    for (int row = osd_y; row < osd_y + osd_h; ++row)
    {
        if (row >= OSD_BUFFER_HEIGHT)
            break; // 边界检查
        uint32_t *line = (uint32_t *)(m_osd_buffer + (row * OSD_BUFFER_WIDTH + osd_x) * 4);
        for (int col = 0; col < osd_w; ++col)
        {
            if (osd_x + col >= OSD_BUFFER_WIDTH)
                break; // 边界检查
            line[col] = bg_color;
        }
    }
}

void OsdManager::draw_text(const std::string &text, int x_start, int y_start)
{
    int pen_x = x_start;
    int pen_y = y_start;

    for (const char &ch : text)
    {
        if (FT_Load_Char(ft_face, ch, FT_LOAD_RENDER))
            continue;

        FT_GlyphSlot g = ft_face->glyph;
        for (int y = 0; y < (int)g->bitmap.rows; y++)
        {
            for (int x = 0; x < (int)g->bitmap.width; x++)
            {
                int img_x = pen_x + g->bitmap_left + x;
                int img_y = pen_y - g->bitmap_top + y;
                if (img_x < 0 || img_x >= OSD_BUFFER_WIDTH || img_y < 0 || img_y >= OSD_BUFFER_HEIGHT)
                    continue;

                unsigned char alpha = g->bitmap.buffer[y * g->bitmap.width + x];
                if (alpha == 0)
                    continue;

                int idx = (img_y * OSD_BUFFER_WIDTH + img_x) * 4;
                // 在半透明背景上绘制白色文字
                m_osd_buffer[idx + 0] = 255;   // R (白色)
                m_osd_buffer[idx + 1] = 255;   // G
                m_osd_buffer[idx + 2] = 255;   // B
                m_osd_buffer[idx + 3] = alpha; // A (文字的不透明度)
            }
        }
        pen_x += (g->advance.x >> 6);
    }
}

void OsdManager::blend_osd_on_frame(AVFrame *frame)
{
    if (!frame || !is_enabled())
        return;
    // [优化] 移除 !frame->data[0] 检查，因为硬件帧的 data[0] 可能是 FD

    std::string line1, line2;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        std::stringstream ss1, ss2;
        ss1 << "Lat: " << std::fixed << std::setprecision(6) << m_pos_data.latitude
            << " Lon: " << m_pos_data.longitude; // [修复] 修正了拼写错误，原为 m::pos_data.longitude
        ss2 << "Speed: " << std::fixed << std::setprecision(1) << m_pos_data.speed_kmh
            << " km/h | " << m_pos_data.timestamp;
        line1 = ss1.str();
        line2 = ss2.str();
    }

    const int osd_x = 50, osd_y = 50; // 左上角边距
    const int line_height = OSD_FONT_SIZE + 10;

    clear_osd_buffer();                                           // 先擦除上次的OSD区域
    draw_background();                                            // 绘制半透明背景
    draw_text(line1, osd_x, osd_y + OSD_FONT_SIZE);               // 在背景上绘制第一行文字
    draw_text(line2, osd_x, osd_y + OSD_FONT_SIZE + line_height); // 绘制第二行

    // --- [优化] 检查帧类型（硬件 vs 软件）并使用正确的RGA导入方法 ---
    rga_buffer_handle_t dst_handle = -1;
    // RK_FORMAT_YCbCr_420_SP 对应 NV12
    const int rga_format = RK_FORMAT_YCbCr_420_SP;

    if (frame->format == AV_PIX_FMT_DRM_PRIME)
    {
        // 这是硬件 (DRM) 帧
        // data[0] 存储的是文件描述符 (fd)
        int frame_fd = (int)(intptr_t)frame->data[0];
        // 使用文件描述符 (FD) 导入RGA缓冲区，实现零拷贝
        dst_handle = importbuffer_fd(frame_fd, frame->width, frame->height, rga_format);
    }
    else if (frame->format == AV_PIX_FMT_NV12)
    {
        // 这是软件 (CPU) 帧 (例如来自 Snapshotter)
        char *frame_data = (char *)frame->data[0];
        // 使用虚拟地址导入RGA缓冲区
        dst_handle = importbuffer_virtualaddr(frame_data, frame->width, frame->height, rga_format);
    }
    else
    {
        // 不支持的帧格式
        fprintf(stderr, "[OSD管理器] 错误: 不支持的帧格式用于OSD叠加: %d\n", frame->format);
        return;
    }

    if (dst_handle <= 0)
    {
        fprintf(stderr, "[OSD管理器] 错误: 导入 RGA 目标句柄失败 (format: %d)\n", frame->format);
        return;
    }

    rga_buffer_t dst_camera_frame = wrapbuffer_handle(dst_handle, frame->width, frame->height, rga_format);
    if (dst_camera_frame.width > 0)
    {
        // 使用 RGA 将 OSD 缓冲区混合到视频帧上 (原地修改)
        imblend(m_rga_src_osd, dst_camera_frame);
    }

    // 释放导入的句柄
    releasebuffer_handle(dst_handle);
}
