/*
 * IMX415 + RK3566 YOLO11 intrusion demo + RTSP streaming
 * Multi-thread low-latency V4L2 handoff version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

extern "C" {
#include "v4l2_capture.h"
#include "mpp_encoder.h"
#include "rtsp_demo.h"
#include <rk_mpi.h>
}

#include "yolo11.h"
#include "image_utils.h"
#include "image_drawing.h"
#include "im2d.h"
#include "drmrga.h"

#define BOUNDARY_Y      540
#define RKNN_MODEL_PATH "/mnt/nfs/rknn_yolo11_demo/model/yolo11.rknn"
#define STAT_INTERVAL   100
#define YOLO_INFER_INTERVAL 3
#define RGA_OVERLAY_THICKNESS 4

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

static inline int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void make_realtime_timeout(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (long)ms * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

typedef struct {
    int64_t wait_us;
    int64_t capture_ts_to_dqbuf_us;
    int64_t handoff_delay_us;
    int64_t rknn_us;
    int64_t memcpy_us;
    int64_t v4l2_return_us;
    int64_t draw_us;
    int64_t encode_us;
    int64_t mpp_prepare_us;
    int64_t mpp_input_wait_us;
    int64_t mpp_hw_wait_us;
    int64_t yolo_preprocess_us;
    int64_t yolo_run_us;
    int64_t yolo_output_us;
    int64_t yolo_postprocess_us;
    int64_t rtsp_tx_us;
    int64_t rtsp_event_us;
    int64_t e2e_send_us;
    int64_t capture_ts_to_rtsp_us;
    int64_t loop_total_us;
    int infer_count;
    int count;
} latency_stat_t;

static void stat_update(latency_stat_t *s,
                        int64_t wait_us, int64_t capture_ts_to_dqbuf_us,
                        int64_t handoff_delay_us,
                        int64_t rknn_us, int64_t memcpy_us,
                        int64_t v4l2_return_us, int64_t draw_us,
                        int64_t encode_us, int64_t mpp_prepare_us,
                        int64_t mpp_input_wait_us, int64_t mpp_hw_wait_us,
                        int64_t yolo_preprocess_us, int64_t yolo_run_us,
                        int64_t yolo_output_us, int64_t yolo_postprocess_us,
                        int64_t rtsp_tx_us, int64_t rtsp_event_us,
                        int64_t e2e_send_us, int64_t capture_ts_to_rtsp_us,
                        int64_t loop_total_us, int did_infer)
{
    s->wait_us += wait_us;
    s->capture_ts_to_dqbuf_us += capture_ts_to_dqbuf_us;
    s->handoff_delay_us += handoff_delay_us;
    s->rknn_us += rknn_us;
    s->memcpy_us += memcpy_us;
    s->v4l2_return_us += v4l2_return_us;
    s->draw_us += draw_us;
    s->encode_us += encode_us;
    s->mpp_prepare_us += mpp_prepare_us;
    s->mpp_input_wait_us += mpp_input_wait_us;
    s->mpp_hw_wait_us += mpp_hw_wait_us;
    s->yolo_preprocess_us += yolo_preprocess_us;
    s->yolo_run_us += yolo_run_us;
    s->yolo_output_us += yolo_output_us;
    s->yolo_postprocess_us += yolo_postprocess_us;
    s->rtsp_tx_us += rtsp_tx_us;
    s->rtsp_event_us += rtsp_event_us;
    s->e2e_send_us += e2e_send_us;
    s->capture_ts_to_rtsp_us += capture_ts_to_rtsp_us;
    s->loop_total_us += loop_total_us;
    s->infer_count += did_infer;
    s->count++;
}

static void stat_print_and_reset(latency_stat_t *s, int frame_idx)
{
    if (s->count == 0) return;
    printf("[latency][threaded_handoff_yolo] frame=%d avg over %d frames, unit=ms\n",
           frame_idx, s->count);
    printf("  wait_v4l2_select+dqbuf: %4lld\n",
           (long long)(s->wait_us / s->count / 1000));
    printf("  capture_ts_to_dqbuf:    %4lld\n",
           (long long)(s->capture_ts_to_dqbuf_us / s->count / 1000));
    printf("  v4l2_handoff_delay:     %4lld\n",
           (long long)(s->handoff_delay_us / s->count / 1000));
    printf("  yolo_actual_infer:      %4d/%d\n",
           s->infer_count, s->count);
    printf("  rknn_inference_avg:     %4lld\n",
           (long long)(s->rknn_us / s->count / 1000));
    printf("  rknn_inference_once:    %4lld\n",
           (long long)(s->rknn_us / (s->infer_count ? s->infer_count : 1) / 1000));
    printf("    yolo_preprocess_rga:  %4lld\n",
           (long long)(s->yolo_preprocess_us / (s->infer_count ? s->infer_count : 1) / 1000));
    printf("    yolo_rknn_run:        %4lld\n",
           (long long)(s->yolo_run_us / (s->infer_count ? s->infer_count : 1) / 1000));
    printf("    yolo_output_convert:  %4lld\n",
           (long long)(s->yolo_output_us / (s->infer_count ? s->infer_count : 1) / 1000));
    printf("    yolo_postprocess:     %4lld\n",
           (long long)(s->yolo_postprocess_us / (s->infer_count ? s->infer_count : 1) / 1000));
    printf("  memcpy_to_mpp:          %4lld\n",
           (long long)(s->memcpy_us / s->count / 1000));
    printf("  v4l2_return_qbuf:       %4lld\n",
           (long long)(s->v4l2_return_us / s->count / 1000));
    printf("  draw_overlay:           %4lld\n",
           (long long)(s->draw_us / s->count / 1000));
    printf("  mpp_encode_total:       %4lld\n",
           (long long)(s->encode_us / s->count / 1000));
    printf("    mpp_prepare:          %4lld\n",
           (long long)(s->mpp_prepare_us / s->count / 1000));
    printf("    mpp_input_wait:       %4lld\n",
           (long long)(s->mpp_input_wait_us / s->count / 1000));
    printf("    mpp_hw_wait:          %4lld\n",
           (long long)(s->mpp_hw_wait_us / s->count / 1000));
    printf("  rtsp_tx_video:          %4lld\n",
           (long long)(s->rtsp_tx_us / s->count / 1000));
    printf("  rtsp_do_event:          %4lld\n",
           (long long)(s->rtsp_event_us / s->count / 1000));
    printf("  e2e_dqbuf_to_rtsp_tx:   %4lld\n",
           (long long)(s->e2e_send_us / s->count / 1000));
    printf("  capture_ts_to_rtsp_tx:  %4lld\n",
           (long long)(s->capture_ts_to_rtsp_us / s->count / 1000));
    printf("  process_loop_total:     %4lld\n",
           (long long)(s->loop_total_us / s->count / 1000));
    fflush(stdout);
    memset(s, 0, sizeof(*s));
}

typedef struct {
    void *data;
    int size;
    int buf_index;
    int dma_fd;
    uint64_t seq;
    int64_t capture_ts_us;
    int64_t dqbuf_us;
    int64_t wait_us;
    int64_t capture_ts_to_dqbuf_us;
} handoff_frame_t;

typedef struct {
    v4l2_ctx_t *v4l2;
    handoff_frame_t ready;
    int has_ready;
    int processing_count;
    uint64_t latest_seq;
    uint64_t dropped_ready_count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} handoff_queue_t;

typedef struct {
    int enabled;
    int width;
    int height;
    int format;
#if defined(LIBRGA_IM2D_HANDLE)
    rga_buffer_handle_t handles[BUF_COUNT];
    rga_buffer_t buffers[BUF_COUNT];
#endif
} rga_overlay_ctx_t;

typedef struct {
    v4l2_ctx_t *v4l2;
    handoff_queue_t *queue;
} capture_thread_arg_t;

typedef struct {
    handoff_queue_t *queue;
    mpp_enc_ctx_t *enc;
    rknn_app_context_t *rknn_ctx;
    rtsp_demo_handle demo;
    rtsp_session_handle session;
    rga_overlay_ctx_t *rga_overlay;
    int width;
    int height;
    int fps;
} process_thread_arg_t;

static void handoff_queue_init(handoff_queue_t *q, v4l2_ctx_t *v4l2)
{
    memset(q, 0, sizeof(*q));
    q->v4l2 = v4l2;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void handoff_queue_deinit(handoff_queue_t *q)
{
    if (q->has_ready) {
        v4l2_queue_buf(q->v4l2, q->ready.buf_index);
        q->has_ready = 0;
    }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->mutex);
}

static void handoff_drop_ready_locked(handoff_queue_t *q)
{
    if (q->has_ready) {
        v4l2_queue_buf(q->v4l2, q->ready.buf_index);
        q->has_ready = 0;
        q->dropped_ready_count++;
    }
}

static int handoff_get_fresh(handoff_queue_t *q, handoff_frame_t *out)
{
    struct timespec ts;
    pthread_mutex_lock(&q->mutex);

    handoff_drop_ready_locked(q);
    while (g_running && !q->has_ready) {
        make_realtime_timeout(&ts, 100);
        pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
    }
    if (!g_running || !q->has_ready) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    *out = q->ready;
    q->has_ready = 0;
    q->processing_count++;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static void handoff_release_frame(handoff_queue_t *q, int buf_index)
{
    pthread_mutex_lock(&q->mutex);
    v4l2_queue_buf(q->v4l2, buf_index);
    if (q->processing_count > 0)
        q->processing_count--;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void rga_overlay_init(rga_overlay_ctx_t *ctx, v4l2_ctx_t *v4l2, int width, int height)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->width = width;
    ctx->height = height;
    ctx->format = RK_FORMAT_YCbCr_420_SP;
    ctx->enabled = 1;

#if defined(LIBRGA_IM2D_HANDLE)
    for (int i = 0; i < BUF_COUNT; i++) {
        im_handle_param_t param;
        memset(&param, 0, sizeof(param));
        param.width = width;
        param.height = height;
        param.format = ctx->format;

        int dma_fd = v4l2->bufs[i].plane[0].dma_fd;
        if (dma_fd < 0) {
            ctx->enabled = 0;
            break;
        }

        ctx->handles[i] = importbuffer_fd(dma_fd, &param);
        if (ctx->handles[i] <= 0) {
            ctx->enabled = 0;
            break;
        }

        ctx->buffers[i] = wrapbuffer_handle(ctx->handles[i],
                                            width, height, ctx->format,
                                            width, height);
    }
#else
    (void)v4l2;
#endif

    if (ctx->enabled) {
        printf("[rga] overlay draw enabled, mode=%s\n",
#if defined(LIBRGA_IM2D_HANDLE)
               "cached-handle"
#else
               "fd-wrap"
#endif
        );
    } else {
        printf("[rga] overlay draw disabled, fallback to CPU draw\n");
    }
}

static void rga_overlay_deinit(rga_overlay_ctx_t *ctx)
{
#if defined(LIBRGA_IM2D_HANDLE)
    for (int i = 0; i < BUF_COUNT; i++) {
        if (ctx->handles[i] > 0) {
            releasebuffer_handle(ctx->handles[i]);
            ctx->handles[i] = 0;
        }
    }
#else
    (void)ctx;
#endif
}

static inline int align_down_even(int v)
{
    return v & ~1;
}

static inline int align_up_even(int v)
{
    return (v + 1) & ~1;
}

static int make_nv12_fill_color(uint8_t y)
{
    int color = 0;
    unsigned char *p = (unsigned char *)&color;
    p[0] = y;
    p[1] = 128;
    p[2] = 128;
    p[3] = 255;
    return color;
}

static int rga_clip_rect_even(im_rect *rect, int img_w, int img_h)
{
    int x0 = rect->x;
    int y0 = rect->y;
    int x1 = rect->x + rect->width;
    int y1 = rect->y + rect->height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img_w) x1 = img_w;
    if (y1 > img_h) y1 = img_h;
    if (x1 <= x0 || y1 <= y0)
        return -1;

    x0 = align_down_even(x0);
    y0 = align_down_even(y0);
    x1 = align_up_even(x1);
    y1 = align_up_even(y1);
    if (x1 > img_w) x1 = img_w;
    if (y1 > img_h) y1 = img_h;
    if ((x1 - x0) < 2 || (y1 - y0) < 2)
        return -1;

    rect->x = x0;
    rect->y = y0;
    rect->width = x1 - x0;
    rect->height = y1 - y0;
    return 0;
}

static int rga_fill_rect(rga_overlay_ctx_t *ctx, int buf_index, int dma_fd,
                         int img_w, int img_h, im_rect rect, int color)
{
    if (!ctx || !ctx->enabled || buf_index < 0 || buf_index >= BUF_COUNT)
        return -1;
    if (rga_clip_rect_even(&rect, img_w, img_h) < 0)
        return 0;

#if defined(LIBRGA_IM2D_HANDLE)
    rga_buffer_t dst = ctx->buffers[buf_index];
#else
    if (dma_fd < 0)
        return -1;
    rga_buffer_t dst = wrapbuffer_fd(dma_fd, img_w, img_h,
                                     RK_FORMAT_YCbCr_420_SP,
                                     img_w, img_h);
#endif

    IM_STATUS ret = imfill(dst, rect, color);
    if (ret <= 0) {
        static int warn_count = 0;
        if (warn_count < 5) {
            printf("[rga] imfill failed: %s\n", imStrError(ret));
            warn_count++;
        }
        return -1;
    }
    return 0;
}

static int rga_draw_rect_nv12(rga_overlay_ctx_t *ctx, int buf_index, int dma_fd,
                              int img_w, int img_h,
                              int x1, int y1, int x2, int y2,
                              uint8_t y_val, int thickness)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= img_w) x2 = img_w - 1;
    if (y2 >= img_h) y2 = img_h - 1;
    if (x1 >= x2 || y1 >= y2)
        return 0;

    int color = make_nv12_fill_color(y_val);
    im_rect top = {x1, y1, x2 - x1 + 1, thickness};
    im_rect bottom = {x1, y2 - thickness + 1, x2 - x1 + 1, thickness};
    im_rect left = {x1, y1, thickness, y2 - y1 + 1};
    im_rect right = {x2 - thickness + 1, y1, thickness, y2 - y1 + 1};

    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, top, color) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, bottom, color) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, left, color) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, right, color) < 0)
        return -1;

    return 0;
}

static void draw_rect_nv12(uint8_t *nv12, int img_w, int img_h,
                           int x1, int y1, int x2, int y2,
                           uint8_t y_val, int thickness)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= img_w) x2 = img_w - 1;
    if (y2 >= img_h) y2 = img_h - 1;
    if (x1 >= x2 || y1 >= y2) return;
    for (int t = 0; t < thickness; t++) {
        int tx1 = x1 + t, ty1 = y1 + t, tx2 = x2 - t, ty2 = y2 - t;
        if (tx1 >= tx2 || ty1 >= ty2) break;
        for (int x = tx1; x <= tx2; x++) {
            nv12[ty1 * img_w + x] = y_val;
            nv12[ty2 * img_w + x] = y_val;
        }
        for (int y = ty1; y <= ty2; y++) {
            nv12[y * img_w + tx1] = y_val;
            nv12[y * img_w + tx2] = y_val;
        }
    }
}

static void draw_detections_cpu(uint8_t *nv12, int img_w, int img_h,
                                const object_detect_result_list *results)
{
    for (int i = 0; i < results->count; i++) {
        const object_detect_result *det = &results->results[i];
        if (det->cls_id != 0) continue;
        int x1 = det->box.left, y1 = det->box.top;
        int x2 = det->box.right, y2 = det->box.bottom;
        int cy = (y1 + y2) / 2;
        if (cy > BOUNDARY_Y) {
            draw_rect_nv12(nv12, img_w, img_h, x1, y1, x2, y2, 16, 3);
            printf("[ALERT] 入侵! person@(%d,%d,%d,%d) 越过y=%d 置信度=%.1f%%\n",
                   x1, y1, x2, y2, BOUNDARY_Y, det->prop * 100.0f);
        } else {
            draw_rect_nv12(nv12, img_w, img_h, x1, y1, x2, y2, 235, 3);
        }
    }
}

static int draw_detections_rga(rga_overlay_ctx_t *ctx, int buf_index, int dma_fd,
                               int img_w, int img_h,
                               const object_detect_result_list *results)
{
    if (!ctx || !ctx->enabled)
        return -1;

    for (int i = 0; i < results->count; i++) {
        const object_detect_result *det = &results->results[i];
        if (det->cls_id != 0) continue;
        int x1 = det->box.left, y1 = det->box.top;
        int x2 = det->box.right, y2 = det->box.bottom;
        int cy = (y1 + y2) / 2;
        uint8_t y_val = (cy > BOUNDARY_Y) ? 64 : 235;

        if (rga_draw_rect_nv12(ctx, buf_index, dma_fd, img_w, img_h,
                               x1, y1, x2, y2, y_val,
                               RGA_OVERLAY_THICKNESS) < 0) {
            return -1;
        }

        if (cy > BOUNDARY_Y) {
            printf("[ALERT] 入侵! person@(%d,%d,%d,%d) 越过y=%d 置信度=%.1f%%\n",
                   x1, y1, x2, y2, BOUNDARY_Y, det->prop * 100.0f);
        }
    }
    return 0;
}

static void *capture_thread_main(void *opaque)
{
    capture_thread_arg_t *arg = (capture_thread_arg_t *)opaque;
    v4l2_ctx_t *v4l2 = arg->v4l2;
    handoff_queue_t *q = arg->queue;

    while (g_running) {
        void *nv12_data = NULL;
        int nv12_size = 0;
        int buf_index = 0;
        int64_t t_wait_begin = now_us();

        if (v4l2_read_frame(v4l2, &nv12_data, &nv12_size, &buf_index) < 0)
            continue;

        int64_t t_dqbuf = now_us();
        int64_t capture_ts = v4l2->last_timestamp_us;
        int64_t capture_ts_to_dqbuf = 0;
        if (capture_ts > 0 && t_dqbuf >= capture_ts)
            capture_ts_to_dqbuf = t_dqbuf - capture_ts;

        pthread_mutex_lock(&q->mutex);
        handoff_drop_ready_locked(q);
        q->latest_seq++;
        q->ready.data = nv12_data;
        q->ready.size = nv12_size;
        q->ready.buf_index = buf_index;
        q->ready.dma_fd = v4l2->bufs[buf_index].plane[0].dma_fd;
        q->ready.seq = q->latest_seq;
        q->ready.capture_ts_us = capture_ts;
        q->ready.dqbuf_us = t_dqbuf;
        q->ready.wait_us = t_dqbuf - t_wait_begin;
        q->ready.capture_ts_to_dqbuf_us = capture_ts_to_dqbuf;
        q->has_ready = 1;
        pthread_cond_signal(&q->cond);
        pthread_mutex_unlock(&q->mutex);
    }

    pthread_mutex_lock(&q->mutex);
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return NULL;
}

static void *process_thread_main(void *opaque)
{
    process_thread_arg_t *arg = (process_thread_arg_t *)opaque;
    handoff_queue_t *q = arg->queue;
    mpp_enc_ctx_t *enc = arg->enc;
    rknn_app_context_t *rknn_ctx = arg->rknn_ctx;
    int width = arg->width;
    int height = arg->height;
    int fps = arg->fps;
    uint64_t ts = rtsp_get_reltime();
    int frame_idx = 0;
    latency_stat_t stat;
    object_detect_result_list cached_results;
    int has_cached_results = 0;

    memset(&stat, 0, sizeof(stat));
    memset(&cached_results, 0, sizeof(cached_results));

    while (g_running) {
        handoff_frame_t frm;
        if (handoff_get_fresh(q, &frm) < 0)
            continue;

        int64_t t_loop_begin = now_us();
        int64_t handoff_delay = (frm.dqbuf_us > 0 && t_loop_begin >= frm.dqbuf_us) ?
                                t_loop_begin - frm.dqbuf_us : 0;

        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(src_image));
        src_image.width = width;
        src_image.height = height;
        src_image.width_stride = width;
        src_image.height_stride = height;
        src_image.format = IMAGE_FORMAT_YUV420SP_NV12;
        src_image.virt_addr = (unsigned char *)frm.data;
        src_image.size = frm.size;
        src_image.fd = frm.dma_fd;

        object_detect_result_list od_results;
        memset(&od_results, 0, sizeof(od_results));
        int did_infer = (!has_cached_results || (frame_idx % YOLO_INFER_INTERVAL) == 0);
        if (did_infer) {
            inference_yolo11_model(rknn_ctx, &src_image, &od_results);
            cached_results = od_results;
            has_cached_results = 1;
        } else {
            od_results = cached_results;
        }
        int64_t t1 = now_us();

        int use_dmabuf_frame = enc->use_dmabuf &&
                               frm.buf_index >= 0 &&
                               frm.buf_index < MAX_BUF_COUNT &&
                               enc->dma_bufs[frm.buf_index] != NULL;
        uint8_t *encode_ptr = NULL;
        int encode_buf_index = -1;
        int64_t memcpy_us = 0;
        int64_t v4l2_return_us = 0;
        int64_t draw_us = 0;
        int64_t encode_us = 0;
        int64_t t2 = 0;
        uint8_t *h264_data = NULL;
        int h264_size = 0;
        const MppPktSeg *seg = NULL;

        if (use_dmabuf_frame) {
            encode_ptr = (uint8_t *)frm.data;
            encode_buf_index = frm.buf_index;

            int64_t t_draw_begin = now_us();
            if (draw_detections_rga(arg->rga_overlay, frm.buf_index, frm.dma_fd,
                                    width, height, &od_results) < 0) {
                draw_detections_cpu(encode_ptr, width, height, &od_results);
            }
            int64_t t_draw_end = now_us();
            draw_us = t_draw_end - t_draw_begin;

            if (mpp_encoder_encode(enc, encode_ptr, frm.size,
                                   encode_buf_index, &h264_data, &h264_size, &seg) < 0) {
                handoff_release_frame(q, frm.buf_index);
                continue;
            }
            t2 = now_us();
            encode_us = t2 - t_draw_end;

            int64_t t_return_begin = now_us();
            handoff_release_frame(q, frm.buf_index);
            int64_t t_return_end = now_us();
            v4l2_return_us = t_return_end - t_return_begin;
        } else {
            encode_ptr = (uint8_t *)mpp_buffer_get_ptr(enc->frm_buf);
            memcpy(encode_ptr, frm.data, (size_t)width * height * 3 / 2);
            int64_t t_memcpy = now_us();
            memcpy_us = t_memcpy - t1;

            int64_t t_return_begin = now_us();
            handoff_release_frame(q, frm.buf_index);
            int64_t t_return_end = now_us();
            v4l2_return_us = t_return_end - t_return_begin;

            int64_t t_draw_begin = now_us();
            draw_detections_cpu(encode_ptr, width, height, &od_results);
            int64_t t_draw_end = now_us();
            draw_us = t_draw_end - t_draw_begin;

            if (mpp_encoder_encode(enc, encode_ptr, width * height * 3 / 2,
                                   -1, &h264_data, &h264_size, &seg) < 0) {
                continue;
            }
            t2 = now_us();
            encode_us = t2 - t_draw_end;
        }

        if (h264_size > 0)
            rtsp_tx_video(arg->session, h264_data, h264_size, ts);
        int64_t t3 = now_us();

        rtsp_do_event(arg->demo);
        int64_t t_event = now_us();
        ts += 1000000 / fps;
        frame_idx++;

        int64_t capture_ts_to_rtsp = 0;
        if (frm.capture_ts_us > 0 && t3 >= frm.capture_ts_us)
            capture_ts_to_rtsp = t3 - frm.capture_ts_us;

        stat_update(&stat,
                    frm.wait_us,
                    frm.capture_ts_to_dqbuf_us,
                    handoff_delay,
                    t1 - t_loop_begin,
                    memcpy_us,
                    v4l2_return_us,
                    draw_us,
                    encode_us,
                    enc->last_prepare_us,
                    enc->last_input_wait_us,
                    enc->last_hw_wait_us,
                    did_infer ? rknn_ctx->last_preprocess_us : 0,
                    did_infer ? rknn_ctx->last_rknn_run_us : 0,
                    did_infer ? rknn_ctx->last_output_convert_us : 0,
                    did_infer ? rknn_ctx->last_postprocess_us : 0,
                    t3 - t2,
                    t_event - t3,
                    t3 - t_loop_begin,
                    capture_ts_to_rtsp,
                    t_event - t_loop_begin,
                    did_infer);

        if (frame_idx % STAT_INTERVAL == 0)
            stat_print_and_reset(&stat, frame_idx);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *dev = CAPTURE_DEV;
    const char *model_path = RKNN_MODEL_PATH;
    int width = CAPTURE_WIDTH;
    int height = CAPTURE_HEIGHT;
    int fps = CAPTURE_FPS;

    if (argc >= 2) dev = argv[1];
    if (argc >= 3) model_path = argv[2];

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int ret = 0;
    v4l2_ctx_t v4l2 = {0};
    mpp_enc_ctx_t enc = {0};
    rga_overlay_ctx_t rga_overlay;
    rknn_app_context_t rknn_ctx;
    rtsp_demo_handle demo = NULL;
    rtsp_session_handle session = NULL;
    handoff_queue_t queue;
    pthread_t capture_tid;
    pthread_t process_tid;
    int v4l2_ok = 0, enc_ok = 0, rknn_ok = 0, queue_ok = 0;
    int capture_started = 0, process_started = 0;

    memset(&rknn_ctx, 0, sizeof(rknn_ctx));
    memset(&queue, 0, sizeof(queue));
    memset(&rga_overlay, 0, sizeof(rga_overlay));

    if (v4l2_open(&v4l2, dev, width, height) < 0) {
        fprintf(stderr, "v4l2_open failed\n");
        ret = -1; goto cleanup;
    }
    v4l2_ok = 1;

    if (mpp_encoder_open(&enc, width, height, fps) < 0) {
        fprintf(stderr, "mpp_encoder_open failed\n");
        ret = -1; goto cleanup;
    }
    enc_ok = 1;

    rga_overlay_init(&rga_overlay, &v4l2, width, height);

    {
        int dma_fds[BUF_COUNT];
        size_t dmabuf_size = v4l2.bufs[0].plane[0].length;
        for (int i = 0; i < BUF_COUNT; i++)
            dma_fds[i] = v4l2.bufs[i].plane[0].dma_fd;
        if (mpp_encoder_setup_dmabuf(&enc, dma_fds, BUF_COUNT, dmabuf_size) == 0) {
            printf("[main] MPP uses V4L2 DMA-BUF directly, NV12 memcpy disabled\n");
        } else {
            printf("[main] MPP DMA-BUF import failed, fallback to NV12 memcpy\n");
        }
    }

    init_post_process();
    if (init_yolo11_model(model_path, &rknn_ctx) != 0) {
        fprintf(stderr, "init_yolo11_model failed\n");
        ret = -1; goto cleanup;
    }
    rknn_ok = 1;
    printf("[main] YOLO11 model loaded: %s\n", model_path);

    demo = rtsp_new_demo(8554);
    if (!demo) { ret = -1; goto cleanup; }
    session = rtsp_new_session(demo, "/live");
    if (!session) { ret = -1; goto cleanup; }
    rtsp_set_video(session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());

    handoff_queue_init(&queue, &v4l2);
    queue_ok = 1;

    printf("========================================\n");
    printf("RTSP: rtsp://YOUR_BOARD_IP:8554/live\n");
    printf("设备: %s  %dx%d@%dfps  越界阈值: y=%d (不显示横线)\n", dev, width, height, fps, BOUNDARY_Y);
    printf("多线程模式: V4L2 handoff + process/encode/rtsp thread\n");
    printf("每%d帧打印一次延迟统计\n", STAT_INTERVAL);
    printf("========================================\n");

    capture_thread_arg_t cap_arg;
    cap_arg.v4l2 = &v4l2;
    cap_arg.queue = &queue;
    process_thread_arg_t proc_arg;
    proc_arg.queue = &queue;
    proc_arg.enc = &enc;
    proc_arg.rknn_ctx = &rknn_ctx;
    proc_arg.demo = demo;
    proc_arg.session = session;
    proc_arg.rga_overlay = &rga_overlay;
    proc_arg.width = width;
    proc_arg.height = height;
    proc_arg.fps = fps;

    if (pthread_create(&capture_tid, NULL, capture_thread_main, &cap_arg) != 0) {
        fprintf(stderr, "pthread_create capture failed\n");
        ret = -1; goto cleanup;
    }
    capture_started = 1;

    if (pthread_create(&process_tid, NULL, process_thread_main, &proc_arg) != 0) {
        fprintf(stderr, "pthread_create process failed\n");
        ret = -1; goto cleanup;
    }
    process_started = 1;

    while (g_running)
        sleep(1);

    printf("\nStopping...\n");

cleanup:
    g_running = 0;
    if (queue_ok) {
        pthread_mutex_lock(&queue.mutex);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.mutex);
    }
    if (capture_started) pthread_join(capture_tid, NULL);
    if (process_started) pthread_join(process_tid, NULL);
    deinit_post_process();
    if (rknn_ok) release_yolo11_model(&rknn_ctx);
    rga_overlay_deinit(&rga_overlay);
    if (session) rtsp_del_session(session);
    if (demo) rtsp_del_demo(demo);
    if (enc_ok) mpp_encoder_close(&enc);
    if (queue_ok) handoff_queue_deinit(&queue);
    if (v4l2_ok) v4l2_close(&v4l2);

    printf("Exit.\n");
    return ret;
}
