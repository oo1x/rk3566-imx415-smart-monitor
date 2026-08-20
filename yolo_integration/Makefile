# IMX415 + YOLO11入侵检测 + RTSP推流
# 交叉编译：make

CROSS   ?= aarch64-linux-gnu-
CC      = $(CROSS)gcc
CXX     = $(CROSS)g++
STRIP   = $(CROSS)strip

# ---- 路径配置 ----
SYSROOT      = /home/oo1/Desktop/Lubancat_kernel/debian/binary
MPP_INC      = $(SYSROOT)/usr/include/rockchip
MPP_LIB      = $(SYSROOT)/usr/lib/aarch64-linux-gnu

RKNN_ZOO     = /home/oo1/Desktop/rknn_model_zoo
RKNN_INC     = rknn_yolo11
RKNN_NPU2    = rknn_yolo11
RKNN_UTILS   = rknn_utils
RKNN_LIB     = $(RKNN_ZOO)/3rdparty/rknpu2/Linux/aarch64/librknnrt.so
RGA_LIB      = $(RKNN_ZOO)/3rdparty/librga/Linux/aarch64/librga.so
RKNN_API_INC = $(RKNN_ZOO)/3rdparty/rknpu2/include
RGA_INC      = $(RKNN_ZOO)/3rdparty/librga/include
SYS_RGA_INC  = $(SYSROOT)/usr/include/rga
STB_INC      = $(RKNN_ZOO)/3rdparty/stb_image
JPEG_INC     = $(RKNN_ZOO)/3rdparty/jpeg_turbo/include
JPEG_LIB     = $(RKNN_ZOO)/3rdparty/jpeg_turbo/Linux/aarch64/libturbojpeg.a
SYS_RGA_LIB  = $(SYSROOT)/usr/lib/aarch64-linux-gnu/librga.so

RTSP_DIR     = ../rtsp_streaming

# ---- 编译选项 ----
CFLAGS  = -g -Wall -O2 -D__LINUX__ -DZERO_COPY --sysroot=$(SYSROOT) \
          -I$(MPP_INC) \
          -I$(RKNN_INC) \
          -I$(RKNN_NPU2) \
          -I$(RKNN_UTILS) \
          -I$(RKNN_API_INC) \
          -I$(RGA_INC) \
          -I$(SYS_RGA_INC) \
          -I$(STB_INC) \
          -I$(JPEG_INC) \
          -I$(RTSP_DIR)

CXXFLAGS = $(CFLAGS) -std=c++11

LDFLAGS = --sysroot=$(SYSROOT) \
          -L$(MPP_LIB) \
          -lrockchip_mpp \
          -lrt \
          -l:libpthread.so.0 \
          -ldl -l:libm.so.6

# ---- 源文件 ----
C_SRCS = $(RTSP_DIR)/rtsp_demo.c \
         $(RTSP_DIR)/rtsp_msg.c \
         $(RTSP_DIR)/rtp_enc.c \
         $(RTSP_DIR)/stream_queue.c \
         $(RTSP_DIR)/utils.c \
         $(RTSP_DIR)/v4l2_capture.c \
         $(RTSP_DIR)/mpp_encoder.c \
         rga_overlay_helper.c \
         $(RKNN_UTILS)/image_utils.c \
         $(RKNN_UTILS)/image_drawing.c \
         $(RKNN_UTILS)/file_utils.c \

CXX_SRCS = imx415_yolo_rtsp.cpp \
           $(RKNN_NPU2)/yolo11_zero_copy.cc \
           $(RKNN_INC)/postprocess.cc

# 把所有源文件的.o统一放在当前目录，用notdir去掉路径
C_OBJS   = $(addprefix obj/, $(notdir $(C_SRCS:.c=.o)))
CXX_OBJS = $(addprefix obj/, $(notdir $(CXX_SRCS:.cpp=.o)))
CXX_OBJS := $(CXX_OBJS:.cc=.o)

TARGET ?= imx415_yolo_rtsp

all: obj $(TARGET)

obj:
	mkdir -p obj

$(TARGET): $(C_OBJS) $(CXX_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ \
		$(RKNN_LIB) \
		$(SYS_RGA_LIB) \
		$(JPEG_LIB) \
		$(LDFLAGS)
	@echo "Build OK: $(TARGET)"

# 当前目录的cpp
obj/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# RTSP和MPP的C文件
obj/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: $(RTSP_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# RKNN utils的C文件
obj/%.o: $(RKNN_UTILS)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# RKNN YOLO11的cc文件
obj/%.o: $(RKNN_NPU2)/%.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf obj $(TARGET)

.PHONY: all clean
