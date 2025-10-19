#include "camera_sdk.h"
#include "camera_controller.h"
#include <iostream>



/**
 * @file camera_sdk.cpp
 * @brief 实现了 camera_sdk.h 中声明的 C 语言 API 接口。
 *
 * 这一层是“桥接层”，它将简单的 C 函数调用转换为对内部 C++ CameraController 对象的成员函数调用。
 * 使用 void* handle (不透明指针) 是 C/C++ 混合编程中隐藏实现细节的标准做法。
 */

extern "C"
{

    void *camera_sdk_create(const char *device_path)
    {
        if (!device_path)
        {
            std::cerr << "SDK错误: 设备路径不能为空。" << std::endl;
            return nullptr;
        }

        try
        {
            // 'new' 一个 C++ 控制器实例
            CameraController *controller = new CameraController(device_path);

            // 调用其初始化方法
            if (!controller->initialize())
            {
                delete controller; // 初始化失败，清理内存并返回空指针
                return nullptr;
            }

            // 将 C++ 对象指针强制转换为 void* 句柄返回给调用者
            return static_cast<void *>(controller);
        }
        catch (const std::bad_alloc &e)
        {
            std::cerr << "SDK错误: 内存分配失败: " << e.what() << std::endl;
            return nullptr;
        }
    }

    void camera_sdk_destroy(void *handle)
    {
        if (handle)
        {
            // 将不透明句柄安全地转换回 C++ 控制器指针
            CameraController *controller = static_cast<CameraController *>(handle);
            // 'delete' C++ 对象，这将自动调用其析构函数，从而安全地释放所有资源
            delete controller;
        }
    }

    int camera_sdk_start_recording(void *handle, const char *resolution)
    {
        if (handle && resolution)
        {
            return static_cast<CameraController *>(handle)->start_recording(resolution);
        }
        return -1;
    }

    int camera_sdk_stop_recording(void *handle)
    {
        if (handle)
        {
            return static_cast<CameraController *>(handle)->stop_recording();
        }
        return -1;
    }

    int camera_sdk_take_snapshot(void *handle)
    {
        if (handle)
        {
            std::cout << "进入camera_sdk_take_snapshot函数" << std::endl;
            return static_cast<CameraController *>(handle)->take_snapshot();
        }
        return -1;
    }

    void camera_sdk_set_osd_enabled(void *handle, bool enabled)
    {
        if (handle)
        {
            static_cast<CameraController *>(handle)->set_osd_enabled(enabled);
        }
    }

    void camera_sdk_zoom_in(void *handle)
    {
        if (handle)
        {
            static_cast<CameraController *>(handle)->zoom_in();
        }
    }

    void camera_sdk_zoom_out(void *handle)
    {
        if (handle)
        {
            static_cast<CameraController *>(handle)->zoom_out();
        }
    }

} // extern "C"
