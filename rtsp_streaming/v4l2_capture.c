#include "v4l2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do { ret = ioctl(fd, req, arg); } while (ret == -1 && errno == EINTR);
    return ret;
}

int v4l2_open(v4l2_ctx_t *ctx, const char *dev, int width, int height)
{
    struct v4l2_capability      cap;
    struct v4l2_format          fmt;
    struct v4l2_requestbuffers  req;
    int i;

    memset(ctx, 0, sizeof(*ctx));
    ctx->width  = width;
    ctx->height = height;

    /* 初始化dma_fd为-1，表示未导出 */
    for (i = 0; i < BUF_COUNT; i++)
        ctx->bufs[i].plane[0].dma_fd = -1;

    ctx->fd = open(dev, O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) { perror("open"); return -1; }

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP"); goto err;
    }
    printf("[v4l2] driver=%s card=%s\n", cap.driver, cap.card);

    /* S_FMT：设置NV12格式 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = width;
    fmt.fmt.pix_mp.height      = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes  = 1;
    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT"); goto err;
    }
    ctx->num_planes = fmt.fmt.pix_mp.num_planes;
    printf("[v4l2] NV12 %dx%d planes=%d\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, ctx->num_planes);

    /* REQBUFS：申请MMAP类型buffer */
    memset(&req, 0, sizeof(req));
    req.count  = BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS"); goto err;
    }

    /* QUERYBUF + mmap + EXPBUF
     * mmap：建立CPU访问的虚拟地址映射（零拷贝访问）
     * EXPBUF：把同一块DMA buffer导出为fd
     *         这个fd可以传给MPP，让硬件编码器直接访问
     *         不需要memcpy */
    for (i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer      buf;
        struct v4l2_plane       planes[VIDEO_MAX_PLANES];
        struct v4l2_exportbuffer expbuf;
        int p;

        memset(&buf,   0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = ctx->num_planes;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF"); goto err;
        }

        for (p = 0; p < ctx->num_planes; p++) {
            /* mmap：建立虚拟地址到DMA物理页的映射 */
            ctx->bufs[i].plane[p].length = planes[p].length;
            ctx->bufs[i].plane[p].start  = mmap(
                NULL, planes[p].length,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                ctx->fd, planes[p].m.mem_offset);
            if (ctx->bufs[i].plane[p].start == MAP_FAILED) {
                perror("mmap"); goto err;
            }

            /* EXPBUF：把同一块DMA buffer导出为DMA-BUF fd
             * 这个fd和mmap指向同一块物理内存
             * 传给MPP后，MPP通过IOMMU直接访问，省掉memcpy */
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            expbuf.index = i;
            expbuf.plane = p;
            expbuf.flags = O_CLOEXEC | O_RDWR;

            if (xioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                /* EXPBUF失败不影响mmap正常工作
                 * 退化为普通mmap模式，由上层决定是否memcpy */
                perror("VIDIOC_EXPBUF (will fallback to memcpy)");
                ctx->bufs[i].plane[p].dma_fd = -1;
            } else {
                ctx->bufs[i].plane[p].dma_fd = expbuf.fd;
                printf("[v4l2] buf[%d] plane[%d] dma_fd=%d size=%d\n",
                       i, p, expbuf.fd, planes[p].length);
            }
        }
    }

    /* QBUF：所有buffer入队 */
    for (i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer  buf;
        struct v4l2_plane   planes[VIDEO_MAX_PLANES];
        memset(&buf,   0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = planes;
        buf.length   = ctx->num_planes;
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF"); goto err;
        }
    }

    /* STREAMON */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON"); goto err;
    }

    printf("[v4l2] opened %s OK\n", dev);
    return 0;
err:
    close(ctx->fd);
    ctx->fd = -1;
    return -1;
}

int v4l2_read_frame(v4l2_ctx_t *ctx, void **data, int *size, int *index)
{
    struct v4l2_buffer  buf, tmp;
    struct v4l2_plane   planes[VIDEO_MAX_PLANES];
    struct v4l2_plane   tmp_planes[VIDEO_MAX_PLANES];
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(ctx->fd, &fds);
    tv.tv_sec = 0; tv.tv_usec = 10000;
    if (select(ctx->fd + 1, &fds, NULL, NULL, &tv) <= 0)
        return -1;

    memset(&buf,   0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length   = ctx->num_planes;
    if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return -1;
        perror("VIDIOC_DQBUF"); return -1;
    }

    /* 丢帧策略：只保留最新帧 */
    while (1) {
        memset(&tmp,       0, sizeof(tmp));
        memset(tmp_planes, 0, sizeof(tmp_planes));
        tmp.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        tmp.memory   = V4L2_MEMORY_MMAP;
        tmp.m.planes = tmp_planes;
        tmp.length   = ctx->num_planes;
        if (xioctl(ctx->fd, VIDIOC_DQBUF, &tmp) < 0) {
            if (errno == EAGAIN) break;
            break;
        }
        xioctl(ctx->fd, VIDIOC_QBUF, &buf);
        buf = tmp;
        memcpy(planes, tmp_planes, sizeof(planes));
    }

    ctx->last_timestamp_us = (int64_t)buf.timestamp.tv_sec * 1000000LL +
                             (int64_t)buf.timestamp.tv_usec;

    *data  = ctx->bufs[buf.index].plane[0].start;
    *size  = planes[0].bytesused ? planes[0].bytesused
                                 : ctx->bufs[buf.index].plane[0].length;
    *index = buf.index;
    return 0;
}

int v4l2_queue_buf(v4l2_ctx_t *ctx, int index)
{
    struct v4l2_buffer  buf;
    struct v4l2_plane   planes[VIDEO_MAX_PLANES];
    memset(&buf,   0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = index;
    buf.m.planes = planes;
    buf.length   = ctx->num_planes;
    return xioctl(ctx->fd, VIDIOC_QBUF, &buf);
}

void v4l2_close(v4l2_ctx_t *ctx)
{
    int i, p;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    for (i = 0; i < BUF_COUNT; i++) {
        for (p = 0; p < ctx->num_planes; p++) {
            if (ctx->bufs[i].plane[p].dma_fd >= 0)
                close(ctx->bufs[i].plane[p].dma_fd);
            if (ctx->bufs[i].plane[p].start &&
                ctx->bufs[i].plane[p].start != MAP_FAILED)
                munmap(ctx->bufs[i].plane[p].start,
                       ctx->bufs[i].plane[p].length);
        }
    }
    close(ctx->fd);
    ctx->fd = -1;
}
