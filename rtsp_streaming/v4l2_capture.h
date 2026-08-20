#ifndef __V4L2_CAPTURE_H__
#define __V4L2_CAPTURE_H__

#include <stdint.h>
#include <stddef.h>
#include <linux/videodev2.h>

#define CAPTURE_WIDTH   1920
#define CAPTURE_HEIGHT  1080
#define CAPTURE_FPS     60
#define CAPTURE_DEV     "/dev/video0"
#define BUF_COUNT       4

typedef struct {
    void   *start;      /* mmap虚拟地址，用于CPU访问 */
    size_t  length;
    int     dma_fd;     /* DMA-BUF fd，用于传给MPP零拷贝编码 */
} v4l2_plane_t;

typedef struct {
    v4l2_plane_t plane[VIDEO_MAX_PLANES];
} v4l2_buf_t;

typedef struct {
    int         fd;
    v4l2_buf_t  bufs[BUF_COUNT];
    int         width;
    int         height;
    int         num_planes;
    int64_t     last_timestamp_us; /* V4L2 buffer timestamp, CLOCK_MONOTONIC us */
} v4l2_ctx_t;

int  v4l2_open(v4l2_ctx_t *ctx, const char *dev, int width, int height);
int  v4l2_read_frame(v4l2_ctx_t *ctx, void **data, int *size, int *index);
int  v4l2_queue_buf(v4l2_ctx_t *ctx, int index);
void v4l2_close(v4l2_ctx_t *ctx);

#endif
