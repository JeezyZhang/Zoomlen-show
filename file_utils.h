#ifndef FILE_UTILS_H
#define FILE_UTILS_H

/*
 * 文件移动函数
 * 优先尝试使用 rename 系统调用，这是最高效的同文件系统移动方式
 * 如果 rename 因跨设备 (EXDEV) 等原因失败，则自动回退到高效的文件复制(使用 sendfile)，复制成功后再删除源文件，从而实现跨文件系统的移动
 * 
 * @param   src_path    源文件路径
 * @param   dst_path    目标文件路径
 * 
 * @return  成功返回 0，失败返回 -1
 */
int move_file_robust(const char *src_path, const char *dst_path);

#endif // FILE_UTILS_H
