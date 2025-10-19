#ifndef ZOOM_MANAGER_H
#define ZOOM_MANAGER_H

#include <mutex>
#include <atomic>

/**
 * @class ZoomManager
 * @brief 线程安全地管理数字变焦状态。
 *
 * - 存储当前的变焦级别。
 * - 根据变焦级别计算 RGA 硬件滤镜所需的裁剪参数 (crop parameters)。
 * - 标记变焦状态是否已改变，以通知其他模块（如录制器）更新其配置。
 */
class ZoomManager {
public:
    ZoomManager();

    // 增加变焦级别 (放大)
    void zoom_in();

    // 减小变焦级别 (缩小)
    void zoom_out();
    
    /**
     * @brief 获取当前的裁剪参数 (线程安全)。
     * @param cx, cy, cw, ch 用于接收裁剪坐标和尺寸的输出参数。
     */
    void get_crop_params(int& cx, int& cy, int& cw, int& ch);

    /**
     * @brief 检查变焦级别是否已改变，并原子地重置标志位。
     * @return 如果自上次检查以来变焦级别已改变，则返回 true。
     */
    bool check_and_reset_change_flag();

private:
    // 根据当前变焦级别更新内部的裁剪参数
    void update_crop_params();

    // 成员变量
    float m_level = 1.0f;              // 当前变焦级别 (1.0x, 1.1x, ...)
    const float m_min_level = 1.0f;    // 最小变焦级别
    const float m_max_level = 8.0f;    // 最大变焦级别
    const float m_step = 0.1f;         // 每次变焦的步长

    // 裁剪参数
    int m_crop_x, m_crop_y, m_crop_w, m_crop_h;

    // 线程安全相关
    std::mutex m_mutex;                // 保护所有成员变量的读写
    std::atomic<bool> m_changed{false};// 变焦状态改变标志
};

#endif // ZOOM_MANAGER_H

