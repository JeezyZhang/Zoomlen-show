#include "file_utils.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <iostream>

/**
 * @file file_utils.cpp
 * @brief 实现了文件操作相关的工具函数。
 */

/**
 * @brief 内部辅助函数: 使用 sendfile 进行高效文件复制。
 * * sendfile 是一个零拷贝或接近零拷贝的系统调用，非常适合在内核空间直接传输文件数据，
 * 效率远高于用户空间的 read/write 循环。
 * * @param src 源文件路径。
 * @param dst 目标文件路径。
 * @return 成功返回 0，失败返回负值。
 */
static int copy_file_sendfile(const char *src, const char *dst)
{
    int in_fd = -1, out_fd = -1;

    in_fd = open(src, O_RDONLY);
    if (in_fd < 0)
    {
        std::cerr << "[文件工具] 错误: 打开源文件失败: " << src << " (" << strerror(errno) << ")" << std::endl;
        return -1;
    }

    struct stat st;
    if (fstat(in_fd, &st) != 0)
    {
        std::cerr << "[文件工具] 错误: 获取源文件状态失败: " << src << " (" << strerror(errno) << ")" << std::endl;
        close(in_fd);
        return -1;
    }

    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (out_fd < 0)
    {
        std::cerr << "[文件工具] 错误: 创建目标文件失败: " << dst << " (" << strerror(errno) << ")" << std::endl;
        close(in_fd);
        return -1;
    }

    off_t offset = 0;
    ssize_t sent = sendfile(out_fd, in_fd, &offset, st.st_size);

    close(in_fd);
    close(out_fd);

    if (sent != st.st_size)
    {
        std::cerr << "[文件工具] 错误: sendfile 复制不完整. 预期 " << st.st_size << ", 实际 " << sent << std::endl;
        unlink(dst); // 删除不完整的目标文件
        return -1;
    }

    return 0;
}

// 对外接口的实现
int move_file_robust(const char *src_path, const char *dst_path)
{
    // 1. 尝试最高效的 rename
    if (rename(src_path, dst_path) == 0)
    {
        return 0;
    }

    // 2. 如果失败原因是跨设备 (EXDEV)，则回退到 "复制+删除" 策略
    if (errno == EXDEV)
    {
        if (copy_file_sendfile(src_path, dst_path) == 0)
        {
            if (unlink(src_path) == 0)
            {
                return 0; // 复制和删除都成功
            }
            else
            {
                std::cerr << "[文件工具] 错误: 复制成功但删除源文件失败: " << src_path << " (" << strerror(errno) << ")" << std::endl;
                return -1;
            }
        }
        // 复制失败
        return -1;
    }

    // 3. 其他类型的 rename 失败
    std::cerr << "[文件工具] 错误: rename 操作失败: " << src_path << " -> " << dst_path << " (" << strerror(errno) << ")" << std::endl;
    return -1;
}
