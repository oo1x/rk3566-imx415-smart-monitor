#ifndef RGA_OVERLAY_HELPER_H
#define RGA_OVERLAY_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

int rga_nv12_fill_rect_fd(int dma_fd, int width, int height,
                          int x, int y, int rect_w, int rect_h,
                          unsigned char y_value);

#ifdef __cplusplus
}
#endif

#endif
