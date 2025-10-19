#ifndef SNAPSHOTTER_H
#define SNAPSHOTTER_H

#include <string>
#include <functional>
#include <memory>

// 向前声明
class OsdManager;
class ZoomManager;

// 媒体文件处理完成后的回调函数类型
using MediaCompleteCallback = std::function<void(const std::string&)>;

/**
 * @class Snapshotter
 * @brief 负责拍照功能的类。
 *
 * 设计为“一次性”使用对象：每次调用 shoot() 都会启动一个后台线程来完成
 * 拍照、保存的完整流程，并在任务结束后自我销毁，避免资源泄露。
 */
class Snapshotter {
public:
    /**
     * @brief 构造函数。
     * @param device 摄像头设备路径。
     * @param osd_manager OSD 管理器的共享指针。
     * @param zoom_manager 变焦管理器的共享指针。
     * @param cb 拍照完成后要调用的回调函数。
     */
    Snapshotter(std::string device,
                std::shared_ptr<OsdManager> osd_manager,
                std::shared_ptr<ZoomManager> zoom_manager,
                MediaCompleteCallback cb);

    /**
     * @brief 触发拍照 (非阻塞)。
     * * 此函数会立即返回，并在后台启动一个新线程来执行拍照任务。
     */
    void shoot();

private:
    // 在独立线程中运行的核心拍照函数
    void run();

    // 成员变量
    std::string m_device_name;
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    MediaCompleteCallback m_on_complete_cb;
};

#endif // SNAPSHOTTER_H

