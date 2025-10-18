#ifndef SNAPSHOTTER_H
#define SNAPSHOTTER_H

#include <string>
#include <functional>
#include <memory>

class OsdManager;
class ZoomManager;

using MediaCompleteCallback = std::function<void(const std::string &)>;

class Snapshotter
{
public:
    Snapshotter(std::string device,
                std::shared_ptr<OsdManager> osd_manager,
                std::shared_ptr<ZoomManager> zoom_manager,
                MediaCompleteCallback cb);

    void shoot();

private:
    void run();

    std::string m_device_name;
    std::shared_ptr<OsdManager> m_osd_manager;
    std::shared_ptr<ZoomManager> m_zoom_manager;
    MediaCompleteCallback m_on_complete_cb;
};

#endif // SNAPSHOTTER_H
