# Makefile for the Camera Recorder Application

# 编译器
CXX = g++

# 包含和库路径
INCLUDES = -I. \
           -I/home/Tronlong/docs/librga/include \
           -I/usr/include/freetype2

LIB_PATHS = -L/home/Tronlong/docs/librga/build/lib

# 编译器和链接器标志
CXXFLAGS = -std=c++11 -Wall -O2 -D_REENTRANT $(INCLUDES)
LDFLAGS = -pthread $(LIB_PATHS)
# 链接所有需要的库: FFmpeg, FreeType, Rockchip MPP, RGA
LDLIBS = $(shell pkg-config --cflags --libs freetype2 libavformat libavcodec libavutil libavdevice libavfilter libswscale) \
         -lrockchip_mpp -lrga

# 更新: 加入 zoom_manager.cpp
SOURCES = main.cpp recorder.cpp file_utils.cpp snapshotter.cpp osd_manager.cpp zoom_manager.cpp

# 对象文件
OBJECTS = $(SOURCES:.cpp=.o)

# 目标可执行文件名
TARGET = video_app

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

