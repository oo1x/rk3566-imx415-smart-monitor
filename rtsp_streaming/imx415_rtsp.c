/*
 * IMX415 + RK3566 RTSP推流demo
 * 优化记录：
 * 1. RKISP直接输出NV12，省掉格式转换
 * 2. memchr替换逐字节NAL扫描，降低CPU占用86%
 * 3. DMA-BUF零拷贝：V4L2 buffer直接导入MPP
 *    省掉每帧约2MB的memcpy（30fps=每秒60MB）
 *    CPU完全不参与V4L2→MPP的数据传输
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "v4l2_capture.h"
#include "mpp_encoder.h"
#include "rtsp_demo.h"

#define LATENCY_STAT_INTERVAL 100

static volatile int g_running = 1;

static inline int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

typedef struct {
    int64_t wait_us;
    int64_t capture_ts_to_dqbuf_us;
    int64_t encode_us;
    int64_t qbuf_us;
    int64_t rtsp_tx_us;
    int64_t rtsp_event_us;
    int64_t e2e_send_us;
    int64_t loop_total_us;
    int count;
} latency_stat_t;

static void latency_stat_update(latency_stat_t *s,
                                int64_t wait_us, int64_t capture_ts_to_dqbuf_us,
                                int64_t encode_us, int64_t qbuf_us, int64_t rtsp_tx_us,
                                int64_t rtsp_event_us, int64_t e2e_send_us,
                                int64_t loop_total_us)
{
    s->wait_us += wait_us;
    s->capture_ts_to_dqbuf_us += capture_ts_to_dqbuf_us;
    s->encode_us += encode_us;
    s->qbuf_us += qbuf_us;
    s->rtsp_tx_us += rtsp_tx_us;
    s->rtsp_event_us += rtsp_event_us;
    s->e2e_send_us += e2e_send_us;
    s->loop_total_us += loop_total_us;
    s->count++;
}

static void latency_stat_print_and_reset(latency_stat_t *s, int frame_idx)
{
    if (s->count == 0)
        return;

    printf("[latency][rtsp] frame=%d avg over %d frames, unit=ms\n",
           frame_idx, s->count);
    printf("  wait_v4l2_select+dqbuf: %4lld\n",
           (long long)(s->wait_us / s->count / 1000));
    printf("  capture_ts_to_dqbuf:    %4lld\n",
           (long long)(s->capture_ts_to_dqbuf_us / s->count / 1000));
    printf("  mpp_encode:             %4lld\n",
           (long long)(s->encode_us / s->count / 1000));
    printf("  v4l2_qbuf:              %4lld\n",
           (long long)(s->qbuf_us / s->count / 1000));
    printf("  rtsp_tx_video:          %4lld\n",
           (long long)(s->rtsp_tx_us / s->count / 1000));
    printf("  rtsp_do_event:          %4lld\n",
           (long long)(s->rtsp_event_us / s->count / 1000));
    printf("  e2e_dqbuf_to_rtsp_tx:   %4lld\n",
           (long long)(s->e2e_send_us / s->count / 1000));
    printf("  loop_total_with_wait:   %4lld\n",
           (long long)(s->loop_total_us / s->count / 1000));
    fflush(stdout);
    memset(s, 0, sizeof(*s));
}

static void sig_handler(int sig)
{
    g_running = 0;
}

int main(int argc, char *argv[])
{
    v4l2_ctx_t          v4l2    = {0};
    mpp_enc_ctx_t       enc     = {0};
    rtsp_demo_handle    demo    = NULL;
    rtsp_session_handle session = NULL;

    const char *dev = CAPTURE_DEV;
    int width       = CAPTURE_WIDTH;
    int height      = CAPTURE_HEIGHT;
    int fps         = CAPTURE_FPS;

    if (argc >= 2) dev = argv[1];
    if (argc >= 4) {
        width  = atoi(argv[2]);
        height = atoi(argv[3]);
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 第一步：打开V4L2摄像头
     * 内部会做EXPBUF，导出每个buffer的dma_fd */
    if (v4l2_open(&v4l2, dev, width, height) < 0) {
        fprintf(stderr, "v4l2_open failed\n");
        return -1;
    }

    /* 第二步：打开MPP硬件编码器 */
    if (mpp_encoder_open(&enc, width, height, fps) < 0) {
        fprintf(stderr, "mpp_encoder_open failed\n");
        v4l2_close(&v4l2);
        return -1;
    }

    /* 第三步：把V4L2的dma_fd导入MPP，建立零拷贝通路
     * 收集所有buffer的dma_fd */
    int dma_fds[BUF_COUNT];
    int i;
    for (i = 0; i < BUF_COUNT; i++)
        dma_fds[i] = v4l2.bufs[i].plane[0].dma_fd;

    size_t buf_size = (size_t)(width * height * 3 / 2);
    if (mpp_encoder_setup_dmabuf(&enc, dma_fds, BUF_COUNT, buf_size) < 0) {
        printf("[main] DMA-BUF setup failed, using memcpy fallback\n");
    }

    /* 第四步：创建RTSP服务器 */
    demo = rtsp_new_demo(8554);
    if (!demo) {
        fprintf(stderr, "rtsp_new_demo failed\n");
        goto cleanup;
    }

    session = rtsp_new_session(demo, "/live");
    if (!session) {
        fprintf(stderr, "rtsp_new_session failed\n");
        goto cleanup;
    }

    rtsp_set_video(session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());

    printf("========================================\n");
    printf("RTSP server started\n");
    printf("Pull stream: rtsp://YOUR_BOARD_IP:8554/live\n");
    printf("Device: %s  Resolution: %dx%d  FPS: %d\n", dev, width, height, fps);
    printf("Mode: %s\n", enc.use_dmabuf ? "DMA-BUF zero-copy" : "memcpy fallback");
    printf("PID: %d  (perf record -g -p %d)\n", getpid(), getpid());
    printf("========================================\n");

    uint64_t ts        = rtsp_get_reltime();
    int      frame_idx = 0;
    latency_stat_t latency_stat = {0};

    while (g_running) {
        void    *nv12_data = NULL;
        int      nv12_size = 0;
        int      buf_index = 0;
        uint8_t *h264_data = NULL;
        int      h264_size = 0;
        const MppPktSeg *seg = NULL;

        int64_t t_loop_begin = now_us();
        if (v4l2_read_frame(&v4l2, &nv12_data, &nv12_size, &buf_index) < 0) {
            rtsp_do_event(demo);
            continue;
        }
        int64_t t_dqbuf_done = now_us();
        int64_t capture_ts_to_dqbuf = 0;
        if (v4l2.last_timestamp_us > 0 && t_dqbuf_done >= v4l2.last_timestamp_us)
            capture_ts_to_dqbuf = t_dqbuf_done - v4l2.last_timestamp_us;

        /* 编码：
         * DMA-BUF模式：buf_index告诉MPP用哪个dma_buf，不拷贝
         * memcpy模式：nv12_data拷贝到MPP buffer */
        if (mpp_encoder_encode(&enc,
                               nv12_data, nv12_size,
                               buf_index,
                               &h264_data, &h264_size,
                               &seg) < 0) {
            v4l2_queue_buf(&v4l2, buf_index);
            continue;
        }
        int64_t t_encode_done = now_us();

        v4l2_queue_buf(&v4l2, buf_index);
        int64_t t_qbuf_done = now_us();

        if (h264_size > 0)
            rtsp_tx_video(session, h264_data, h264_size, ts);
        int64_t t_rtsp_tx_done = now_us();

        rtsp_do_event(demo);
        int64_t t_event_done = now_us();

        ts += 1000000 / fps;
        frame_idx++;

        latency_stat_update(&latency_stat,
                            t_dqbuf_done - t_loop_begin,
                            capture_ts_to_dqbuf,
                            t_encode_done - t_dqbuf_done,
                            t_qbuf_done - t_encode_done,
                            t_rtsp_tx_done - t_qbuf_done,
                            t_event_done - t_rtsp_tx_done,
                            t_rtsp_tx_done - t_dqbuf_done,
                            t_event_done - t_loop_begin);

        if (frame_idx % LATENCY_STAT_INTERVAL == 0) {
            printf("encoded %d frames [%s]\n",
                   frame_idx,
                   enc.use_dmabuf ? "zero-copy" : "memcpy");
            latency_stat_print_and_reset(&latency_stat, frame_idx);
        }
    }

    printf("\nStopping...\n");

cleanup:
    if (session) rtsp_del_session(session);
    if (demo)    rtsp_del_demo(demo);
    mpp_encoder_close(&enc);
    v4l2_close(&v4l2);

    printf("Exit.\n");
    return 0;
}
