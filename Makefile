#
# Makefile for the Camera SDK and Example Application
#
# 这个 Makefile 用于构建一个静态库 (libcamera_sdk.a) 和一个使用该库的示例程序。
# 它将所有中间编译文件 (.o) 放入 'build/obj' 目录，保持项目根目录的整洁。
#

# 编译器和归档工具
CXX = g++
AR = ar

# --- 路径配置 ---
# 定义目标文件(.o)的输出目录
OBJ_DIR = build/obj
# 定义库文件(.a)的输出目录
LIB_DIR = build/lib
# 定义最终交付文件(头文件和库)的安装目录
INSTALL_DIR = install

# 包含和库路径 (请根据您的环境修改)
INCLUDES = -I. \
           -I/home/Tronlong/docs/librga/include \
           -I/usr/include/freetype2 \
           -I/usr/include

LIB_PATHS = -L/home/Tronlong/docs/librga/build/lib

# --- 编译和链接标志 ---
# 使用 pkg-config 分别获取编译 (cflags) 和链接 (libs) 标志
PKG_CFLAGS = $(shell pkg-config --cflags freetype2 libavformat libavcodec libavutil libavdevice libavfilter libswscale)
PKG_LIBS = $(shell pkg-config --libs freetype2 libavformat libavcodec libavutil libavdevice libavfilter libswscale)

# CXXFLAGS: C++ 编译器标志
# -std=c++11: 使用 C++11 标准
# -Wall: 开启所有常用警告
# -O2: 优化级别
# -fPIC: 生成位置无关代码，这是创建共享库或在某些架构上创建静态库所必需的
# -D_REENTRANT: 为多线程程序定义预处理器宏
# 将 pkg-config 获取的头文件路径 (-I...) 添加到编译器标志中
CXXFLAGS = -std=c++11 -Wall -O2 -fPIC -D_REENTRANT $(INCLUDES) $(PKG_CFLAGS)

# LDFLAGS: 链接器标志
# -pthread: 链接 POSIX 线程库
LDFLAGS = -pthread $(LIB_PATHS)

# LDLIBS: 需要链接的库
# 使用 pkg-config 工具自动获取 FFmpeg 和 FreeType 的编译和链接标志
# -lrockchip_mpp, -lim2d, -lrga: 链接 Rockchip 平台相关的硬件加速库
# 这里只保留链接库相关的标志 (-L..., -l...)
LDLIBS = $(PKG_LIBS) -lrockchip_mpp -lrga

# --- SDK 库定义 ---
# 组成静态库的所有源文件 (除了示例 main.cpp)
SDK_SOURCES = camera_sdk.cpp camera_controller.cpp recorder.cpp file_utils.cpp snapshotter.cpp osd_manager.cpp zoom_manager.cpp
# 根据源文件列表自动生成对应的 .o 文件路径列表
SDK_OBJECTS = $(addprefix $(OBJ_DIR)/, $(SDK_SOURCES:.cpp=.o))
SDK_TARGET_NAME = libcamera_sdk.a
SDK_TARGET = $(LIB_DIR)/$(SDK_TARGET_NAME)

# --- 示例程序定义 ---
EXAMPLE_SOURCE = example_main.cpp
EXAMPLE_OBJECT = $(addprefix $(OBJ_DIR)/, $(EXAMPLE_SOURCE:.cpp=.o))
EXAMPLE_TARGET = example_app

# --- 公共头文件 ---
PUBLIC_HEADER = camera_sdk.h

# --- 伪目标 ---
# .PHONY 告诉 make，这些目标不是真正的文件名
.PHONY: all clean install

# --- 主要规则 ---
# 'make all' 或直接 'make' 会执行此规则
# 它依赖于示例程序，而示例程序又依赖于静态库，从而确保了正确的构建顺序
all: $(EXAMPLE_TARGET)

# 构建示例程序的规则
$(EXAMPLE_TARGET): $(EXAMPLE_OBJECT) $(SDK_TARGET)
	@echo "===> 链接示例程序: $@"
	$(CXX) $(LDFLAGS) -o $@ $(EXAMPLE_OBJECT) -L$(LIB_DIR) -lcamera_sdk $(LDLIBS)

# 构建静态库的规则
$(SDK_TARGET): $(SDK_OBJECTS)
	@echo "===> 创建静态库: $@"
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $^

# 通用编译规则：如何从 .cpp 文件生成 .o 文件
$(OBJ_DIR)/%.o: %.cpp
	@echo "===> 编译: $<"
	@mkdir -p $(@D) # '@D' 代表目标文件的目录部分，确保目录存在
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 安装规则：将交付给甲方的文件复制到 install 目录
install: $(SDK_TARGET)
	@mkdir -p $(INSTALL_DIR)/include
	@mkdir -p $(INSTALL_DIR)/lib
	cp $(PUBLIC_HEADER) $(INSTALL_DIR)/include/
	cp $(SDK_TARGET) $(INSTALL_DIR)/lib/
	@echo "--------------------------------------------------"
	@echo "SDK 已安装到 '$(INSTALL_DIR)' 目录:"
	@echo "  头文件: $(INSTALL_DIR)/include/$(PUBLIC_HEADER)"
	@echo "  静态库: $(INSTALL_DIR)/lib/$(SDK_TARGET_NAME)"
	@echo "--------------------------------------------------"

# 清理规则：删除所有生成的文件
clean:
	@echo "===> 清理所有生成的文件..."
	rm -rf build $(EXAMPLE_TARGET) $(INSTALL_DIR)

