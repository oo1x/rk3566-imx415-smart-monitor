#include <stdio.h>
#include <string.h>

#include "im2d.h"
#include "drmrga.h"
#include "rga_overlay_helper.h"

#ifndef RGA_OVERLAY_LOG_ENABLE
#define RGA_OVERLAY_LOG_ENABLE 0
#endif

#if RGA_OVERLAY_LOG_ENABLE
#define RGA_OVERLAY_LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define RGA_OVERLAY_LOG(...) do { } while (0)
#endif

static int make_nv12_fill_color(unsigned char y_value)
{
    int color = 0;
    unsigned char *p = (unsigned char *)&color;
    p[0] = y_value;
    p[1] = 128;
    p[2] = 128;
    p[3] = 255;
    return color;
}

int rga_nv12_fill_rect_fd(int dma_fd, int width, int height,
                          int x, int y, int rect_w, int rect_h,
                          unsigned char y_value)
{
    if (dma_fd < 0 || width <= 0 || height <= 0 || rect_w <= 0 || rect_h <= 0)
        return -1;

    rga_buffer_t dst = wrapbuffer_fd(dma_fd, width, height,
                                     RK_FORMAT_YCbCr_420_SP,
                                     width, height);
    im_rect rect = {x, y, rect_w, rect_h};
    int color = make_nv12_fill_color(y_value);
    int ret = imfill(dst, rect, color);
    if (ret <= 0) {
        static int warn_count = 0;
        if (warn_count < 5) {
            RGA_OVERLAY_LOG("[rga] imfill failed: %s\n", imStrError((IM_STATUS)ret));
            warn_count++;
        }
        return -1;
    }

    return 0;
}
