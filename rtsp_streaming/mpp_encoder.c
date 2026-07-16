#include "mpp_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MPP_LOG_ENABLE
#define MPP_LOG_ENABLE 0
#endif

#if MPP_LOG_ENABLE
#define MPP_LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define MPP_LOG(...) do { } while (0)
#endif

static int64_t mpp_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int mpp_encoder_open(mpp_enc_ctx_t *enc, int width, int height, int fps)
{
    MPP_RET   ret;
    MppEncCfg cfg = NULL;

    memset(enc, 0, sizeof(*enc));
    enc->width      = width;
    enc->height     = height;
    enc->hor_stride = MPP_ALIGN(width,  16);
    enc->ver_stride = MPP_ALIGN(height, 16);
    enc->fps        = fps;
    enc->use_dmabuf = 0;  /* 榛樿memcpy妯″紡锛宻etup_dmabuf鎴愬姛鍚庡垏鎹?*/

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_create failed: %d\n", ret);
        return -1;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_init failed: %d\n", ret);
        goto err;
    }

    mpp_enc_cfg_init(&cfg);
    enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, cfg);

    mpp_enc_cfg_set_s32(cfg, "prep:width",       width);
    mpp_enc_cfg_set_s32(cfg, "prep:height",      height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",  enc->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",  enc->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format",      MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg, "rc:mode",           MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target",     4 * 1024 * 1024);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",        6 * 1024 * 1024);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",        2 * 1024 * 1024);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex",    0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",     fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm",  1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex",   0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",    fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",            fps);

    mpp_enc_cfg_set_s32(cfg, "codec:type",    MPP_VIDEO_CodingAVC);
    /* 浣庡欢杩熶紭鍏堬細Baseline + CAVLC锛岄檷浣庣‖浠剁紪鐮佸鏉傚害銆?     * 浠ｄ环鏄悓鐮佺巼涓嬪帇缂╂晥鐜囦綆浜?High Profile + CABAC銆?*/
    mpp_enc_cfg_set_s32(cfg, "h264:profile",  66);
    mpp_enc_cfg_set_s32(cfg, "h264:level",    41);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 0);

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) {
        fprintf(stderr, "MPP_ENC_SET_CFG failed\n");
        mpp_enc_cfg_deinit(cfg);
        goto err;
    }
    mpp_enc_cfg_deinit(cfg);

    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) { fprintf(stderr, "buf_grp failed\n"); goto err; }

    /* fallback鐢ㄧ殑frm_buf锛孌MA-BUF涓嶅彲鐢ㄦ椂鐢╩emcpy */
    ret = mpp_buffer_get(enc->buf_grp, &enc->frm_buf,
                         enc->hor_stride * enc->ver_stride * 3 / 2);
    if (ret != MPP_OK) { fprintf(stderr, "frm_buf failed\n"); goto err; }

    ret = mpp_buffer_get(enc->buf_grp, &enc->pkt_buf,
                         enc->hor_stride * enc->ver_stride * 3 / 2);
    if (ret != MPP_OK) { fprintf(stderr, "pkt_buf failed\n"); goto err; }

    mpp_packet_init_with_buffer(&enc->packet, enc->pkt_buf);

    MPP_LOG("[mpp] encoder opened %dx%d fps=%d\n", width, height, fps);
    return 0;
err:
    mpp_destroy(enc->ctx);
    enc->ctx = NULL;
    return -1;
}

int mpp_encoder_setup_dmabuf(mpp_enc_ctx_t *enc,
                              int *dma_fds, int count, size_t buf_size)
{
    int i;

    if (!dma_fds || count <= 0) return -1;

    for (i = 0; i < count && i < MAX_BUF_COUNT; i++) {
        if (dma_fds[i] < 0) {
            fprintf(stderr, "[mpp] dma_fd[%d] invalid\n", i);
            goto fallback;
        }

        /* 瀵煎叆V4L2鐨凞MA-BUF fd
         * MPP閫氳繃IOMMU鎶婅繖鍧楃墿鐞嗗唴瀛樻槧灏勭粰纭欢缂栫爜鍣?         * 缂栫爜鏃剁‖浠剁洿鎺ヨ鍙栵紝涓嶉渶瑕丆PU鍋歮emcpy */
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DRM;
        info.fd   = dma_fds[i];
        info.size = buf_size;

        MPP_RET ret = mpp_buffer_import(&enc->dma_bufs[i], &info);
        if (ret != MPP_OK) {
            fprintf(stderr, "[mpp] import dma_fd[%d]=%d failed: %d\n",
                    i, dma_fds[i], ret);
            goto fallback;
        }
        MPP_LOG("[mpp] imported dma_fd[%d]=%d size=%zu OK\n",
               i, dma_fds[i], buf_size);
    }

    enc->use_dmabuf = 1;
    MPP_LOG("[mpp] DMA-BUF zero-copy mode enabled\n");
    return 0;

fallback:
    /* 瀵煎叆澶辫触锛岄噴鏀惧凡瀵煎叆鐨勶紝閫€鍖栦负memcpy妯″紡 */
    for (i = 0; i < MAX_BUF_COUNT; i++) {
        if (enc->dma_bufs[i]) {
            mpp_buffer_put(enc->dma_bufs[i]);
            enc->dma_bufs[i] = NULL;
        }
    }
    enc->use_dmabuf = 0;
    MPP_LOG("[mpp] fallback to memcpy mode\n");
    return -1;
}

int mpp_encoder_encode(mpp_enc_ctx_t *enc,
                       void *nv12_data, int nv12_size,
                       int buf_index,
                       uint8_t **h264_data, int *h264_size,
                       const MppPktSeg **seg_info)
{
    MPP_RET   ret;
    MppTask   task = NULL;
    MppBuffer input_buf;
    uint8_t *frm_ptr;
    int64_t t_begin, t_prepare, t_input_ready, t_output_ready;

    enc->last_prepare_us = 0;
    enc->last_input_wait_us = 0;
    enc->last_hw_wait_us = 0;
    t_begin = mpp_now_us();

    if (enc->use_dmabuf && buf_index >= 0 &&
        enc->dma_bufs[buf_index]) {
        /* 闆舵嫹璐濇ā寮忥細鐩存帴鐢╒4L2 DMA-BUF瀵煎叆鐨刡uffer
         * 纭欢缂栫爜鍣ㄩ€氳繃IOMMU鐩存帴璁块棶V4L2鐨勭墿鐞嗗唴瀛?         * CPU瀹屽叏涓嶅弬涓庢暟鎹紶杈?*/
        input_buf = enc->dma_bufs[buf_index];
        /* 涓嶉渶瑕乵emcpy锛?*/
    } else {
        /* memcpy妯″紡锛坒allback锛夛細
         * 濡傛灉涓婂眰宸茬粡鐩存帴鍐欏叆enc->frm_buf锛岃繖閲屼笉鍐嶅仛鍐椾綑memcpy銆?*/
        frm_ptr = (uint8_t *)mpp_buffer_get_ptr(enc->frm_buf);
        if (nv12_data && nv12_data != frm_ptr) {
            memcpy(frm_ptr, nv12_data,
                   enc->hor_stride * enc->ver_stride * 3 / 2);
        }
        input_buf = enc->frm_buf;
    }

    /* 姣忓抚閲嶇疆packet */
    mpp_packet_set_pos(enc->packet,
                       mpp_buffer_get_ptr(enc->pkt_buf));
    mpp_packet_set_length(enc->packet, 0);

    /* 鏋勯€燤ppFrame */
    mpp_frame_init(&enc->frame);
    mpp_frame_set_width(enc->frame,      enc->width);
    mpp_frame_set_height(enc->frame,     enc->height);
    mpp_frame_set_hor_stride(enc->frame, enc->hor_stride);
    mpp_frame_set_ver_stride(enc->frame, enc->ver_stride);
    mpp_frame_set_fmt(enc->frame,        MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(enc->frame,     input_buf);
    mpp_frame_set_eos(enc->frame,        0);

    /* Some RK3566 MPP builds do not reliably honor rc:gop for periodic IDR.
     * Force one IDR per configured second so a late RTSP client can acquire
     * a decoder reference point instead of receiving P-frames forever. */
    if (enc->frame_count == 0 ||
        (enc->fps > 0 && (enc->frame_count % enc->fps) == 0)) {
        ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_IDR_FRAME, NULL);
        if (ret != MPP_OK)
            fprintf(stderr, "MPP_ENC_SET_IDR_FRAME failed: %d\n", ret);
    }

    t_prepare = mpp_now_us();

    /* 鎻愪氦缂栫爜浠诲姟 */
    ret = enc->mpi->poll(enc->ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) return -1;
    ret = enc->mpi->dequeue(enc->ctx, MPP_PORT_INPUT, &task);
    if (ret != MPP_OK) return -1;

    mpp_task_meta_set_frame (task, KEY_INPUT_FRAME,   enc->frame);
    mpp_task_meta_set_packet(task, KEY_OUTPUT_PACKET, enc->packet);

    ret = enc->mpi->enqueue(enc->ctx, MPP_PORT_INPUT, task);
    if (ret != MPP_OK) return -1;
    t_input_ready = mpp_now_us();

    /* 绛夊緟纭欢缂栫爜瀹屾垚 */
    ret = enc->mpi->poll(enc->ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) return -1;
    ret = enc->mpi->dequeue(enc->ctx, MPP_PORT_OUTPUT, &task);
    if (ret != MPP_OK) return -1;

    t_output_ready = mpp_now_us();
    enc->last_prepare_us = t_prepare - t_begin;
    enc->last_input_wait_us = t_input_ready - t_prepare;
    enc->last_hw_wait_us = t_output_ready - t_input_ready;

    mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &enc->packet);

    *h264_data = mpp_packet_get_pos(enc->packet);
    *h264_size = mpp_packet_get_length(enc->packet);

    if (seg_info)
        *seg_info = mpp_packet_get_segment_info(enc->packet);

    enc->mpi->enqueue(enc->ctx, MPP_PORT_OUTPUT, task);
    enc->frame_count++;
    return 0;
}

void mpp_encoder_close(mpp_enc_ctx_t *enc)
{
    int i;
    for (i = 0; i < MAX_BUF_COUNT; i++) {
        if (enc->dma_bufs[i]) {
            mpp_buffer_put(enc->dma_bufs[i]);
            enc->dma_bufs[i] = NULL;
        }
    }
    if (enc->packet)   mpp_packet_deinit(&enc->packet);
    if (enc->pkt_buf)  mpp_buffer_put(enc->pkt_buf);
    if (enc->frm_buf)  mpp_buffer_put(enc->frm_buf);
    if (enc->buf_grp)  mpp_buffer_group_put(enc->buf_grp);
    if (enc->ctx)      mpp_destroy(enc->ctx);
    memset(enc, 0, sizeof(*enc));
    MPP_LOG("[mpp] encoder closed\n");
}
