#include "zoom_manager.h"
#include "app_config.h"

#include <iostream>

ZoomManager::ZoomManager()
{
    // 使用配置中的传感器分辨率初始化
    update_crop_params();
}

void ZoomManager::zoom_in()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_level < m_max_level)
    {
        m_level += m_step;
        update_crop_params();
        m_changed = true;
        printf("[变焦管理器] 变焦级别: %.1fx\n", m_level);
    }
}

void ZoomManager::zoom_out()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_level > m_min_level)
    {
        m_level -= m_step;
        if (m_level < m_min_level)
            m_level = m_min_level;
        update_crop_params();
        m_changed = true;
        printf("[变焦管理器] 变焦级别: %.1fx\n", m_level);
    }
}

void ZoomManager::get_crop_params(int &cx, int &cy, int &cw, int &ch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    cx = m_crop_x;
    cy = m_crop_y;
    cw = m_crop_w;
    ch = m_crop_h;
}

bool ZoomManager::check_and_reset_change_flag()
{
    // exchange 是一个原子操作, 它会返回 m_changed 的旧值, 并将其设为新值 (false)
    return m_changed.exchange(false);
}

void ZoomManager::update_crop_params()
{
    // 注意: 此函数应在已持有锁的情况下被调用
    int src_w = V4L2_INPUT_WIDTH;
    int src_h = V4L2_INPUT_HEIGHT;

    m_crop_w = static_cast<int>(src_w / m_level);
    m_crop_h = static_cast<int>(src_h / m_level);
    m_crop_x = (src_w - m_crop_w) / 2;
    m_crop_y = (src_h - m_crop_h) / 2;

    // 确保裁剪参数是偶数，这对于视频处理（特别是YUV格式）很重要
    m_crop_x &= ~1;
    m_crop_y &= ~1;
    m_crop_w &= ~1;
    m_crop_h &= ~1;
}
