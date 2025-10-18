#include "recorder.h"          // 包含录制器头文件，用于视频录制功能
#include "file_utils.h"        // 包含文件工具头文件，用于文件移动操作
#include "app_config.h"        // 包含应用配置头文件，定义各种配置参数
#include "snapshotter.h"       // 包含快照器头文件，用于拍照功能
#include "osd_manager.h"       // 包含OSD管理器头文件，用于屏幕叠加显示
#include "zoom_manager.h"      // 包含变焦管理器头文件，用于数字变焦控制

#include <iostream>            // 包含标准输入输出流库
#include <thread>              // 包含线程库，用于多线程操作
#include <string>              // 包含字符串库
#include <vector>              // 包含向量容器库
#include <mutex>               // 包含互斥锁库，用于线程同步
#include <condition_variable>  // 包含条件变量库，用于线程间通信
#include <queue>               // 包含队列容器库
#include <cstring>             // 包含C字符串处理函数
#include <memory>              // 包含智能指针库

extern "C"
{
#include <libavdevice/avdevice.h>  // 包含FFmpeg设备库，用于设备输入输出
#include <libavutil/log.h>         // 包含FFmpeg日志库，用于日志输出
}

// 全局的、线程安全的文件移动任务队列
std::queue<std::string>     g_file_move_queue;              // 定义全局文件移动任务队列：存储待移动的文件路径
std::mutex                  g_queue_mutex;                  // 定义全局互斥锁：保护 文件移动任务队列 的线程安全
std::condition_variable     g_cv;                           // 定义全局条件变量：用于线程间通信
std::atomic<bool>           g_exit_file_manager(false);     // 定义原子布尔变量：用于通知文件管理器线程退出

// 文件管理器线程的主函数
void file_manager_thread_func()
{
    // 循环执行文件移动任务，直到收到退出信号
    while (!g_exit_file_manager)
    {
        std::string src_path;
        {
            // 创建互斥锁的唯一锁，用于保护共享资源
            std::unique_lock<std::mutex> lock(g_queue_mutex);

            // 等待条件变量，直到队列非空或收到退出信号
            // Lambda表达式作为等待条件
            g_cv.wait(lock, []
                      { return !g_file_move_queue.empty() || g_exit_file_manager; });

            // 如果收到退出信号且队列为空，则退出循环          
            if (g_exit_file_manager && g_file_move_queue.empty())
                break;

            // 如果队列非空，则取出队列中的第一个文件路径
            if (!g_file_move_queue.empty())
            {
                src_path = g_file_move_queue.front();  // 获取队列首元素
                g_file_move_queue.pop();               // 从队列中移除首元素
            }
        }
        
        // 如果源文件路径非空，则执行文件移动操作
        if (!src_path.empty())
        {   
            // 查找源文件路径中最后一个'/'字符的位置
            const char *slash = strrchr(src_path.c_str(), '/');

            // 获取文件名部分
            const char *fname = slash ? slash + 1 : src_path.c_str();

            // 构造目标文件路径
            std::string dst_path = FINAL_STORAGE_PATH + std::string(fname);

            printf("[文件管理器] 正在移动 %s -> %s\n", src_path.c_str(), dst_path.c_str());

            int ret = move_file_robust(src_path.c_str(), dst_path.c_str());
            if (ret == 0)
            {
                printf("[文件管理器] 移动成功: %s\n", dst_path.c_str());
            }
            else
            {
                fprintf(stderr, "[文件管理器] 错误: 移动文件 %s 失败\n", src_path.c_str());
            }
        }
    }
    printf("[文件管理器] 线程正在退出\n");
}

// 通用的回调函数，当录制或拍照在临时目录完成后被调用
void on_media_finished(const std::string &temp_filepath)
{
    // 使用锁保护共享资源
    std::lock_guard<std::mutex> lock(g_queue_mutex);

    // 将完成的文件路径添加到文件移动队列中
    g_file_move_queue.push(temp_filepath);

    // 通知等待的线程有新任务
    g_cv.notify_one();
}

// 打印用户操作提示
void print_usage()
{
    std::cout << "========= 摄像头应用 ==========" << std::endl;
    std::cout << "  record <res>  - 录制一段视频 (1080p, 720p, 360p)" << std::endl;
    std::cout << "  snapshot      - 拍摄一张照片 (1080p)" << std::endl;
    std::cout << "  osd on/off    - OSD 开启/关闭" << std::endl;
    std::cout << "  +/-           - 数码变倍 放大/缩小 (步长 0.1x)" << std::endl;
    std::cout << "  record stop   - 录制完毕" << std::endl;
    std::cout << "  exit          - 退出程序" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "> ";
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "用法: %s /dev/videoX\n", argv[0]);
        return -1;
    }
    const char *device_name = argv[1];

    // 设置FFmpeg日志级别为错误级别
    av_log_set_level(AV_LOG_ERROR);

    // 注册所有设备
    avdevice_register_all();

    // 创建并初始化所有管理器

    // 创建OSD管理器的智能指针
    auto osd_manager = std::make_shared<OsdManager>();
    if (!osd_manager->initialize())
    {
        std::cerr << "错误: OSD 管理器初始化失败，程序退出" << std::endl;
        return -1;
    }

    // 创建变焦管理器的智能指针
    auto zoom_manager = std::make_shared<ZoomManager>();

    // 启动后台线程

    // 创建文件管理器线程
    std::thread file_manager_thread(file_manager_thread_func);

    // 创建录制器实例，并传入所有管理器
    // 创建录制器对象，传入设备名称、OSD管理器、变焦管理器和完成回调函数
    Recorder recorder(device_name, osd_manager, zoom_manager, on_media_finished);

    // 定义录制线程变量
    std::thread rec_thread;

    print_usage();
    
    std::string line;

    // 用户命令处理循环
    while (std::getline(std::cin, line))
    {
        if (line.rfind("record ", 0) == 0)
        {
            // 提取录制命令的参数
            std::string cmd = line.substr(7);
            if (cmd == "stop")
            {
                if (recorder.isRecording())
                {
                    std::cout << "[录制器] 正在停止录制..." << std::endl;
                    recorder.stop();

                    // 如果录制线程可连接，则等待其结束
                    if (rec_thread.joinable())
                    {
                        rec_thread.join();
                    }
                    std::cout << "[录制器] 录制已停止" << std::endl;
                }
                else
                {
                    std::cerr << "错误: 当前没有在录制" << std::endl;
                }
            }
            else
            {
                if (recorder.isRecording())
                {
                    std::cerr << "错误: 录制已在进行中" << std::endl;
                }
                else
                {
                    if (recorder.prepare(cmd))
                    {
                        // 创建录制线程并启动录制
                        rec_thread = std::thread([&recorder]()
                                                 { recorder.run(); });
                    }
                }
            }
        }
        else if (line == "snapshot")
        {
            printf("[拍照器] 请求拍照...\n");

            // 创建快照器对象
            auto *snapshotter = new Snapshotter(device_name, osd_manager, zoom_manager, on_media_finished);
            
            // 触发拍照操作
            snapshotter->shoot();
        }
        else if (line == "osd on")
        {
            osd_manager->enable(true);
        }
        else if (line == "osd off")
        {
            osd_manager->enable(false);
        }
        else if (line == "+")
        {
            zoom_manager->zoom_in();
        }
        else if (line == "-")
        {
            zoom_manager->zoom_out();
        }
        else if (line == "exit")
        {
            if (recorder.isRecording())
            {
                recorder.stop();

                // 如果录制线程可连接，则等待其结束
                if (rec_thread.joinable())
                {
                    rec_thread.join();
                }
            }

            break;
        }
        else if (!line.empty())
        {
            std::cerr << "未知命令: " << line << std::endl;
        }
        
        // 如果没有在录制，则打印提示符
        if (!recorder.isRecording())
        {
            std::cout << "> ";
        }
    }

    // 程序清理和退出
    osd_manager->shutdown();
    g_exit_file_manager = true;
    g_cv.notify_one();
    if (file_manager_thread.joinable())
    {
        file_manager_thread.join();
    }

    std::cout << "资源已释放, 程序已退出" << std::endl;
    return 0;
}
