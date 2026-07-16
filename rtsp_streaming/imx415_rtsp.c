/*
 * IMX415 + RK3566 RTSP鎺ㄦ祦demo
 * 浼樺寲璁板綍锛? * 1. RKISP鐩存帴杈撳嚭NV12锛岀渷鎺夋牸寮忚浆鎹? * 2. memchr鏇挎崲閫愬瓧鑺侼AL鎵弿锛岄檷浣嶤PU鍗犵敤86%
 * 3. DMA-BUF闆舵嫹璐濓細V4L2 buffer鐩存帴瀵煎叆MPP
 *    鐪佹帀姣忓抚绾?MB鐨刴emcpy锛?0fps=姣忕60MB锛? *    CPU瀹屽叏涓嶅弬涓嶸4L2鈫扢PP鐨勬暟鎹紶杈? */

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
#define RTSP_APP_LOG_ENABLE 0

#if RTSP_APP_LOG_ENABLE
#define RTSP_APP_LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define RTSP_APP_LOG(...) do { } while (0)
#endif

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

    RTSP_APP_LOG("[latency][rtsp] frame=%d avg over %d frames, unit=ms\n",
           frame_idx, s->count);
    RTSP_APP_LOG("  wait_v4l2_select+dqbuf: %4lld\n",
           (long long)(s->wait_us / s->count / 1000));
    RTSP_APP_LOG("  capture_ts_to_dqbuf:    %4lld\n",
           (long long)(s->capture_ts_to_dqbuf_us / s->count / 1000));
    RTSP_APP_LOG("  mpp_encode:             %4lld\n",
           (long long)(s->encode_us / s->count / 1000));
    RTSP_APP_LOG("  v4l2_qbuf:              %4lld\n",
           (long long)(s->qbuf_us / s->count / 1000));
    RTSP_APP_LOG("  rtsp_tx_video:          %4lld\n",
           (long long)(s->rtsp_tx_us / s->count / 1000));
    RTSP_APP_LOG("  rtsp_do_event:          %4lld\n",
           (long long)(s->rtsp_event_us / s->count / 1000));
    RTSP_APP_LOG("  e2e_dqbuf_to_rtsp_tx:   %4lld\n",
           (long long)(s->e2e_send_us / s->count / 1000));
    RTSP_APP_LOG("  loop_total_with_wait:   %4lld\n",
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

    /* 绗竴姝ワ細鎵撳紑V4L2鎽勫儚澶?     * 鍐呴儴浼氬仛EXPBUF锛屽鍑烘瘡涓猙uffer鐨刣ma_fd */
    if (v4l2_open(&v4l2, dev, width, height) < 0) {
        fprintf(stderr, "v4l2_open failed\n");
        return -1;
    }

    /* 绗簩姝ワ細鎵撳紑MPP纭欢缂栫爜鍣?*/
    if (mpp_encoder_open(&enc, width, height, fps) < 0) {
        fprintf(stderr, "mpp_encoder_open failed\n");
        v4l2_close(&v4l2);
        return -1;
    }

    /* 绗笁姝ワ細鎶奦4L2鐨刣ma_fd瀵煎叆MPP锛屽缓绔嬮浂鎷疯礉閫氳矾
     * 鏀堕泦鎵€鏈塨uffer鐨刣ma_fd */
    int dma_fds[BUF_COUNT];
    int i;
    for (i = 0; i < BUF_COUNT; i++)
        dma_fds[i] = v4l2.bufs[i].plane[0].dma_fd;

    size_t buf_size = (size_t)(width * height * 3 / 2);
    if (mpp_encoder_setup_dmabuf(&enc, dma_fds, BUF_COUNT, buf_size) < 0) {
        RTSP_APP_LOG("[main] DMA-BUF setup failed, using memcpy fallback\n");
    }

    /* 绗洓姝ワ細鍒涘缓RTSP鏈嶅姟鍣?*/
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

    RTSP_APP_LOG("========================================\n");
    RTSP_APP_LOG("RTSP server started\n");
    RTSP_APP_LOG("Pull stream: rtsp://YOUR_BOARD_IP:8554/live\n");
    RTSP_APP_LOG("Device: %s  Resolution: %dx%d  FPS: %d\n", dev, width, height, fps);
    RTSP_APP_LOG("Mode: %s\n", enc.use_dmabuf ? "DMA-BUF zero-copy" : "memcpy fallback");
    RTSP_APP_LOG("PID: %d  (perf record -g -p %d)\n", getpid(), getpid());
    RTSP_APP_LOG("========================================\n");

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

        /* 缂栫爜锛?         * DMA-BUF妯″紡锛歜uf_index鍛婅瘔MPP鐢ㄥ摢涓猟ma_buf锛屼笉鎷疯礉
         * memcpy妯″紡锛歯v12_data鎷疯礉鍒癕PP buffer */
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
            RTSP_APP_LOG("encoded %d frames [%s]\n",
                   frame_idx,
                   enc.use_dmabuf ? "zero-copy" : "memcpy");
            latency_stat_print_and_reset(&latency_stat, frame_idx);
        }
    }

    RTSP_APP_LOG("\nStopping...\n");

cleanup:
    if (session) rtsp_del_session(session);
    if (demo)    rtsp_del_demo(demo);
    mpp_encoder_close(&enc);
    v4l2_close(&v4l2);

    RTSP_APP_LOG("Exit.\n");
    return 0;
}
