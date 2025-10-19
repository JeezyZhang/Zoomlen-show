#include "osd_manager.h"
#include "app_config.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring> // For memset

// FFmpeg 头文件
extern "C" {
#include <libavutil/frame.h>
}

/**
 * @file osd_manager.cpp
 * @brief 实现了 OsdManager 类的功能。
 */

OsdManager::OsdManager() {
    // 初始化模拟数据
    m_device_info = {22.54, 114.05, 55.0, "2025-10-18 14:07:00"};
}

OsdManager::~OsdManager() {
    // 确保在对象析构时，资源一定会被释放
    if (!m_shutdown_flag) {
        shutdown();
    }
}

bool OsdManager::initialize() {
    if (!init_freetype()) return false;
    if (!init_rga()) return false;
    
    m_shutdown_flag = false;
    m_data_thread = std::thread(&OsdManager::data_update_thread_func, this);
    
    std::cout << "[OSD管理器] 初始化成功。" << std::endl;
    return true;
}

void OsdManager::shutdown() {
    m_shutdown_flag = true;
    if (m_data_thread.joinable()) {
        m_data_thread.join();
    }
    cleanup_rga();
    cleanup_freetype();
    std::cout << "[OSD管理器] 已关闭。" << std::endl;
}

void OsdManager::enable(bool state) {
    m_enabled = state;
    std::cout << "[OSD管理器] OSD 功能已 " << (state ? "开启" : "关闭") << std::endl;
}

bool OsdManager::is_enabled() const {
    return m_enabled;
}

void OsdManager::data_update_thread_func() {
    while (!m_shutdown_flag) {
        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            // 更新模拟数据
            m_device_info.latitude += 0.00001;
            m_device_info.longitude += 0.00002;
            m_device_info.speed_kmh = 50.0f + (rand() % 200 - 100) / 10.0f; // 40-60 km/h

            // 获取并格式化当前时间
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
            m_device_info.timestamp = ss.str();
        }
        // 每秒更新一次
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool OsdManager::init_freetype() {
    if (FT_Init_FreeType(&ft_library)) {
        std::cerr << "[OSD管理器] 错误: 初始化 FreeType 库失败\n";
        return false;
    }
    if (FT_New_Face(ft_library, OSD_FONT_PATH, 0, &ft_face)) {
        std::cerr << "[OSD管理器] 错误: 加载字体失败: " << OSD_FONT_PATH << "\n";
        FT_Done_FreeType(ft_library);
        return false;
    }
    FT_Set_Pixel_Sizes(ft_face, 0, OSD_FONT_SIZE);
    return true;
}

void OsdManager::cleanup_freetype() {
    if (ft_face) FT_Done_Face(ft_face);
    if (ft_library) FT_Done_FreeType(ft_library);
    ft_face = nullptr;
    ft_library = nullptr;
}

bool OsdManager::init_rga() {
    int bpp = 4; // RGBA8888
    m_osd_buffer = (char*)malloc(OSD_BUFFER_WIDTH * OSD_BUFFER_HEIGHT * bpp);
    if (!m_osd_buffer) {
        std::cerr << "[OSD管理器] 错误: 分配 OSD 缓冲区内存失败。" << std::endl;
        return false;
    }
    // 初始时清空为全透明
    memset(m_osd_buffer, 0, OSD_BUFFER_WIDTH * OSD_BUFFER_HEIGHT * bpp);

    m_rga_src_handle = importbuffer_virtualaddr(m_osd_buffer, OSD_BUFFER_WIDTH, OSD_BUFFER_HEIGHT, RK_FORMAT_RGBA_8888);
    if (m_rga_src_handle <= 0) {
        std::cerr << "[OSD管理器] 错误: 导入 OSD 缓冲区到 RGA 失败!\n";
        return false;
    }

    m_rga_src_osd = wrapbuffer_handle(m_rga_src_handle, OSD_BUFFER_WIDTH, OSD_BUFFER_HEIGHT, RK_FORMAT_RGBA_8888);
    if(m_rga_src_osd.width == 0) {
        fprintf(stderr, "[OSD管理器] 错误: 包装 RGA OSD 句柄失败: %s\n", imStrError());
        return false;
    }
    return true;
}

void OsdManager::cleanup_rga() {
    if (m_rga_src_handle > 0) releasebuffer_handle(m_rga_src_handle);
    if (m_osd_buffer) {
        free(m_osd_buffer);
        m_osd_buffer = nullptr;
    }
    m_rga_src_handle = -1;
}

void OsdManager::clear_dirty_rect() {
    // 定义一个足够大的“脏矩形”区域来覆盖两行文字
    const int osd_x = 40, osd_y = 40;
    const int osd_w = 900, osd_h = OSD_FONT_SIZE * 2 + 20;
    for (int row = osd_y; row < osd_y + osd_h; ++row) {
        memset(m_osd_buffer + (row * OSD_BUFFER_WIDTH + osd_x) * 4, 0, osd_w * 4);
    }
}

void OsdManager::draw_text(const std::string& text, int x_start, int y_start) {
    int pen_x = x_start;
    int pen_y = y_start;

    for (const char &ch : text) {
        if (FT_Load_Char(ft_face, ch, FT_LOAD_RENDER)) continue;

        FT_GlyphSlot g = ft_face->glyph;
        for (int y = 0; y < (int)g->bitmap.rows; y++) {
            for (int x = 0; x < (int)g->bitmap.width; x++) {
                int img_x = pen_x + g->bitmap_left + x;
                int img_y = pen_y - g->bitmap_top + y;
                if (img_x < 0 || img_x >= OSD_BUFFER_WIDTH || img_y < 0 || img_y >= OSD_BUFFER_HEIGHT) continue;

                unsigned char alpha = g->bitmap.buffer[y * g->bitmap.width + x];
                if (alpha == 0) continue;

                int idx = (img_y * OSD_BUFFER_WIDTH + img_x) * 4;
                m_osd_buffer[idx + 0] = 255;   // R (白色)
                m_osd_buffer[idx + 1] = 255;   // G
                m_osd_buffer[idx + 2] = 255;   // B
                m_osd_buffer[idx + 3] = alpha; // A
            }
        }
        pen_x += (g->advance.x >> 6);
    }
}

void OsdManager::blend_osd_on_frame(AVFrame *frame) {
    if (!frame || !frame->data[0] || !is_enabled()) return;
    
    std::string line1, line2;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        std::stringstream ss1, ss2;
        ss1 << "Lat: " << std::fixed << std::setprecision(6) << m_device_info.latitude
            << " Lon: " << m_device_info.longitude;
        ss2 << "Speed: " << std::fixed << std::setprecision(1) << m_device_info.speed_kmh
            << " km/h | " << m_device_info.timestamp;
        line1 = ss1.str();
        line2 = ss2.str();
    }

    const int osd_x = 40, osd_y = 40;
    const int line_height = OSD_FONT_SIZE + 10;

    clear_dirty_rect();                             // 先擦除旧内容
    draw_text(line1, osd_x, osd_y + OSD_FONT_SIZE); // 绘制新内容
    draw_text(line2, osd_x, osd_y + OSD_FONT_SIZE + line_height);

    char* frame_data = (char*)frame->data[0];
    rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(frame_data, frame->width, frame->height, RK_FORMAT_YCbCr_420_SP);
    if (dst_handle <= 0) return;

    rga_buffer_t dst_camera_frame = wrapbuffer_handle(dst_handle, frame->width, frame->height, RK_FORMAT_YCbCr_420_SP);
    if (dst_camera_frame.width > 0) {
        imblend(m_rga_src_osd, dst_camera_frame);
    }
    
    releasebuffer_handle(dst_handle);
}

