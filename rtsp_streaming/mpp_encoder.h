#ifndef __MPP_ENCODER_H__
#define __MPP_ENCODER_H__

#include <stdint.h>

#include <rk_mpi.h>
#include <mpp_frame.h>
#include <mpp_packet.h>

#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#endif

#define MAX_BUF_COUNT 4

typedef struct {
    MppCtx         ctx;
    MppApi        *mpi;
    MppPacket      packet;
    MppFrame       frame;
    MppBufferGroup buf_grp;

    /* 输入帧buffer：
     * 优先用dma_buf（从V4L2导入的DMA-BUF，零拷贝）
     * fallback用frm_buf（自己申请的DRM buffer，需要memcpy）*/
    MppBuffer      dma_bufs[MAX_BUF_COUNT]; /* 从V4L2 fd导入 */
    MppBuffer      frm_buf;                 /* fallback用 */
    int            use_dmabuf;              /* 1=零拷贝，0=memcpy */

    MppBuffer      pkt_buf;
    int            width;
    int            height;
    int            hor_stride;
    int            ver_stride;
    int            frame_count;
    int            fps;

    /* last-frame fine-grained encoder timing, unit: us */
    int64_t        last_prepare_us;
    int64_t        last_input_wait_us;
    int64_t        last_hw_wait_us;
} mpp_enc_ctx_t;

int  mpp_encoder_open(mpp_enc_ctx_t *enc, int width, int height, int fps);

/* dma_fds：V4L2导出的DMA-BUF fd数组，传入后MPP直接导入零拷贝
 * 如果dma_fds为NULL或导入失败，退化为memcpy模式 */
int  mpp_encoder_setup_dmabuf(mpp_enc_ctx_t *enc,
                               int *dma_fds, int count, size_t buf_size);

int  mpp_encoder_encode(mpp_enc_ctx_t *enc,
                        void *nv12_data, int nv12_size,
                        int buf_index,
                        uint8_t **h264_data, int *h264_size,
                        const MppPktSeg **seg_info);

void mpp_encoder_close(mpp_enc_ctx_t *enc);

#endif
