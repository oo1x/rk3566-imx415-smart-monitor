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
#include "rga_overlay_helper.h"

#define BOUNDARY_Y      540
#define RKNN_MODEL_PATH "/mnt/nfs/rknn_yolo11_demo/model/yolo11.rknn"
#define STAT_INTERVAL   100
#define YOLO_INFER_INTERVAL 3
#define RGA_OVERLAY_THICKNESS 4
#define APP_LOG_ENABLE 0

#if APP_LOG_ENABLE
#define APP_LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define APP_LOG(...) do { } while (0)
#endif

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

static FILE *g_metrics_fp = NULL;
static pthread_mutex_t g_metrics_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_ai_enabled = 1;
static int g_infer_interval = YOLO_INFER_INTERVAL;
static int g_reuse_boxes = 1;
static int g_detailed_trace = 1;
static int g_infer_queue_limit = 8;
static const char *g_ai_start_file = NULL;
static const char *g_queue_status_path = NULL;
static int g_ai_activation_seen = 0;
static int64_t g_last_status_us = 0;

typedef enum {
    SUBMIT_DISABLED = 0,
    SUBMIT_ALWAYS_QUEUE = 1,
    SUBMIT_LATEST_IF_IDLE = 2,
} submit_policy_t;

static submit_policy_t g_submit_policy = SUBMIT_LATEST_IF_IDLE;

static void metrics_row(const char *event, uint64_t frame_id, int64_t capture_ts,
                        int64_t rga_start, int64_t rga_end, int64_t infer_submit,
                        int64_t infer_start, int64_t infer_end, int64_t post_end,
                        int64_t overlay_start, int64_t overlay_end,
                        int64_t encode_submit, int64_t encode_output,
                        uint64_t detection_source_frame_id, int triggered, int skipped,
                        const char *skip_reason, int queue_depth)
{
    if (!g_metrics_fp || !g_detailed_trace) return;
    pthread_mutex_lock(&g_metrics_mutex);
    fprintf(g_metrics_fp,
            "%s,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%llu,%d,%d,%s,%d\n",
            event, (unsigned long long)frame_id, (long long)capture_ts,
            (long long)rga_start, (long long)rga_end, (long long)infer_submit,
            (long long)infer_start, (long long)infer_end, (long long)post_end,
            (long long)overlay_start, (long long)overlay_end,
            (long long)encode_submit, (long long)encode_output,
            (unsigned long long)detection_source_frame_id, triggered, skipped,
            skip_reason ? skip_reason : "", queue_depth);
    pthread_mutex_unlock(&g_metrics_mutex);
}

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
#if APP_LOG_ENABLE
    APP_LOG("[latency][threaded_handoff_yolo] frame=%d avg over %d frames, unit=ms\n",
           frame_idx, s->count);
    APP_LOG("  wait_v4l2_select+dqbuf: %4lld\n",
           (long long)(s->wait_us / s->count / 1000));
    APP_LOG("  capture_ts_to_dqbuf:    %4lld\n",
           (long long)(s->capture_ts_to_dqbuf_us / s->count / 1000));
    APP_LOG("  v4l2_handoff_delay:     %4lld\n",
           (long long)(s->handoff_delay_us / s->count / 1000));
    APP_LOG("  yolo_actual_infer:      %4d/%d\n",
           s->infer_count, s->count);
    APP_LOG("  rknn_inference_avg:     %4lld\n",
           (long long)(s->rknn_us / s->count / 1000));
    APP_LOG("  rknn_inference_once:    %4lld\n",
           (long long)(s->rknn_us / (s->infer_count ? s->infer_count : 1) / 1000));
    APP_LOG("    yolo_preprocess_rga:  %4lld\n",
           (long long)(s->yolo_preprocess_us / (s->infer_count ? s->infer_count : 1) / 1000));
    APP_LOG("    yolo_rknn_run:        %4lld\n",
           (long long)(s->yolo_run_us / (s->infer_count ? s->infer_count : 1) / 1000));
    APP_LOG("    yolo_output_convert:  %4lld\n",
           (long long)(s->yolo_output_us / (s->infer_count ? s->infer_count : 1) / 1000));
    APP_LOG("    yolo_postprocess:     %4lld\n",
           (long long)(s->yolo_postprocess_us / (s->infer_count ? s->infer_count : 1) / 1000));
    APP_LOG("  memcpy_to_mpp:          %4lld\n",
           (long long)(s->memcpy_us / s->count / 1000));
    APP_LOG("  v4l2_return_qbuf:       %4lld\n",
           (long long)(s->v4l2_return_us / s->count / 1000));
    APP_LOG("  draw_overlay:           %4lld\n",
           (long long)(s->draw_us / s->count / 1000));
    APP_LOG("  mpp_encode_total:       %4lld\n",
           (long long)(s->encode_us / s->count / 1000));
    APP_LOG("    mpp_prepare:          %4lld\n",
           (long long)(s->mpp_prepare_us / s->count / 1000));
    APP_LOG("    mpp_input_wait:       %4lld\n",
           (long long)(s->mpp_input_wait_us / s->count / 1000));
    APP_LOG("    mpp_hw_wait:          %4lld\n",
           (long long)(s->mpp_hw_wait_us / s->count / 1000));
    APP_LOG("  rtsp_tx_video:          %4lld\n",
           (long long)(s->rtsp_tx_us / s->count / 1000));
    APP_LOG("  rtsp_do_event:          %4lld\n",
           (long long)(s->rtsp_event_us / s->count / 1000));
    APP_LOG("  e2e_dqbuf_to_rtsp_tx:   %4lld\n",
           (long long)(s->e2e_send_us / s->count / 1000));
    APP_LOG("  capture_ts_to_rtsp_tx:  %4lld\n",
           (long long)(s->capture_ts_to_rtsp_us / s->count / 1000));
    APP_LOG("  process_loop_total:     %4lld\n",
           (long long)(s->loop_total_us / s->count / 1000));
    fflush(stdout);
#else
    (void)frame_idx;
#endif
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
} rga_overlay_ctx_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} overlay_rect_t;

typedef struct {
    v4l2_ctx_t *v4l2;
    handoff_queue_t *queue;
} capture_thread_arg_t;

typedef struct {
    rknn_app_context_t *rknn_ctx;
    unsigned char **queue_bufs;
    unsigned char *work_buf;
    uint64_t *queue_seq;
    int64_t *queue_capture_ts_us;
    int64_t *queue_submit_ts_us;
    size_t frame_size;
    int width;
    int height;
    int queue_capacity;
    int queue_head;
    int queue_tail;
    int queue_count;
    int busy;
    object_detect_result_list latest_results;
    uint64_t latest_source_seq;
    int has_results;
    uint64_t submitted_count;
    uint64_t skipped_count;
    uint64_t completed_count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ai_async_ctx_t;

typedef struct {
    handoff_queue_t *queue;
    mpp_enc_ctx_t *enc;
    ai_async_ctx_t *ai;
    rtsp_demo_handle demo;
    rtsp_session_handle session;
    rga_overlay_ctx_t *rga_overlay;
    int width;
    int height;
    int fps;
} process_thread_arg_t;

static int ai_async_init(ai_async_ctx_t *ctx, rknn_app_context_t *rknn_ctx,
                         int width, int height)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->rknn_ctx = rknn_ctx;
    ctx->width = width;
    ctx->height = height;
    ctx->frame_size = (size_t)width * height * 3 / 2;
    ctx->queue_capacity = g_infer_queue_limit > 0 ? g_infer_queue_limit : 1;
    if (ctx->queue_capacity > 64)
        ctx->queue_capacity = 64;
    ctx->queue_bufs = (unsigned char **)calloc((size_t)ctx->queue_capacity,
                                               sizeof(*ctx->queue_bufs));
    ctx->queue_seq = (uint64_t *)calloc((size_t)ctx->queue_capacity,
                                        sizeof(*ctx->queue_seq));
    ctx->queue_capture_ts_us = (int64_t *)calloc((size_t)ctx->queue_capacity,
                                                 sizeof(*ctx->queue_capture_ts_us));
    ctx->queue_submit_ts_us = (int64_t *)calloc((size_t)ctx->queue_capacity,
                                                sizeof(*ctx->queue_submit_ts_us));
    ctx->work_buf = (unsigned char *)malloc(ctx->frame_size);
    if (ctx->queue_bufs) {
        for (int i = 0; i < ctx->queue_capacity; i++) {
            ctx->queue_bufs[i] = (unsigned char *)malloc(ctx->frame_size);
            if (!ctx->queue_bufs[i])
                break;
        }
    }
    int allocation_ok = ctx->queue_bufs && ctx->queue_seq &&
                        ctx->queue_capture_ts_us && ctx->queue_submit_ts_us &&
                        ctx->work_buf;
    for (int i = 0; allocation_ok && i < ctx->queue_capacity; i++)
        if (!ctx->queue_bufs[i]) allocation_ok = 0;
    if (!allocation_ok) {
        if (ctx->queue_bufs)
            for (int i = 0; i < ctx->queue_capacity; i++)
                free(ctx->queue_bufs[i]);
        free(ctx->queue_bufs);
        free(ctx->queue_seq);
        free(ctx->queue_capture_ts_us);
        free(ctx->queue_submit_ts_us);
        free(ctx->work_buf);
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    return 0;
}

static void ai_async_deinit(ai_async_ctx_t *ctx)
{
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
    if (ctx->queue_bufs)
        for (int i = 0; i < ctx->queue_capacity; i++)
            free(ctx->queue_bufs[i]);
    free(ctx->queue_bufs);
    free(ctx->queue_seq);
    free(ctx->queue_capture_ts_us);
    free(ctx->queue_submit_ts_us);
    free(ctx->work_buf);
    memset(ctx, 0, sizeof(*ctx));
}

static int ai_async_submit(ai_async_ctx_t *ctx, const void *nv12,
                           uint64_t seq, int64_t capture_ts_us,
                           int64_t submit_ts_us, int *queue_depth)
{
    if (!ctx || !nv12)
        return 0;
    pthread_mutex_lock(&ctx->mutex);
    if (g_submit_policy == SUBMIT_LATEST_IF_IDLE &&
        (ctx->busy || ctx->queue_count > 0)) {
        ctx->skipped_count++;
        *queue_depth = ctx->queue_count;
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
    }
    if (ctx->queue_count >= ctx->queue_capacity) {
        ctx->skipped_count++;
        *queue_depth = ctx->queue_count;
        pthread_mutex_unlock(&ctx->mutex);
        return -2;
    }
    int slot = ctx->queue_tail;
    memcpy(ctx->queue_bufs[slot], nv12, ctx->frame_size);
    ctx->queue_seq[slot] = seq;
    ctx->queue_capture_ts_us[slot] = capture_ts_us;
    ctx->queue_submit_ts_us[slot] = submit_ts_us;
    ctx->queue_tail = (ctx->queue_tail + 1) % ctx->queue_capacity;
    ctx->queue_count++;
    ctx->submitted_count++;
    *queue_depth = ctx->queue_count;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
    return 1;
}

static int ai_is_active(void)
{
    if (!g_ai_enabled || g_submit_policy == SUBMIT_DISABLED)
        return 0;
    if (!g_ai_start_file || !g_ai_start_file[0])
        return 1;
    if (!g_ai_activation_seen && access(g_ai_start_file, F_OK) == 0)
        g_ai_activation_seen = 1;
    return g_ai_activation_seen;
}

static void ai_async_get_results(ai_async_ctx_t *ctx,
                                 object_detect_result_list *results,
                                 uint64_t *source_seq, int *has_results)
{
    pthread_mutex_lock(&ctx->mutex);
    *has_results = ctx->has_results;
    *source_seq = ctx->latest_source_seq;
    if (ctx->has_results)
        *results = ctx->latest_results;
    else
        memset(results, 0, sizeof(*results));
    pthread_mutex_unlock(&ctx->mutex);
}

static int ai_async_queue_depth(ai_async_ctx_t *ctx)
{
    int depth;
    pthread_mutex_lock(&ctx->mutex);
    depth = ctx->queue_count;
    pthread_mutex_unlock(&ctx->mutex);
    return depth;
}

static void maybe_write_queue_status(ai_async_ctx_t *ctx, uint64_t frame_id, int force)
{
    if (!g_queue_status_path || !g_queue_status_path[0] || !ctx)
        return;
    int64_t timestamp = now_us();
    pthread_mutex_lock(&g_status_mutex);
    if (!force && g_last_status_us > 0 && timestamp - g_last_status_us < 1000000) {
        pthread_mutex_unlock(&g_status_mutex);
        return;
    }
    g_last_status_us = timestamp;

    pthread_mutex_lock(&ctx->mutex);
    int queue_depth = ctx->queue_count;
    int busy = ctx->busy;
    uint64_t submitted = ctx->submitted_count;
    uint64_t skipped = ctx->skipped_count;
    uint64_t completed = ctx->completed_count;
    pthread_mutex_unlock(&ctx->mutex);

    char temporary[1024];
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d",
             g_queue_status_path, (int)getpid());
    FILE *fp = fopen(temporary, "w");
    if (fp) {
        fprintf(fp,
                "monotonic_us=%lld frame_id=%llu infer_queue_depth=%d "
                "infer_busy=%d infer_submitted=%llu infer_skipped=%llu "
                "infer_completed=%llu\n",
                (long long)timestamp, (unsigned long long)frame_id,
                queue_depth, busy, (unsigned long long)submitted,
                (unsigned long long)skipped, (unsigned long long)completed);
        fclose(fp);
        rename(temporary, g_queue_status_path);
    }
    pthread_mutex_unlock(&g_status_mutex);
}

static void *ai_thread_main(void *opaque)
{
    ai_async_ctx_t *ctx = (ai_async_ctx_t *)opaque;
    while (g_running) {
        uint64_t seq;
        int64_t capture_ts_us;
        int64_t submit_ts_us;
        unsigned char *tmp;
        pthread_mutex_lock(&ctx->mutex);
        while (g_running && ctx->queue_count == 0)
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        if (!g_running) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }
        int slot = ctx->queue_head;
        tmp = ctx->work_buf;
        ctx->work_buf = ctx->queue_bufs[slot];
        ctx->queue_bufs[slot] = tmp;
        seq = ctx->queue_seq[slot];
        capture_ts_us = ctx->queue_capture_ts_us[slot];
        submit_ts_us = ctx->queue_submit_ts_us[slot];
        ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_capacity;
        ctx->queue_count--;
        ctx->busy = 1;
        pthread_mutex_unlock(&ctx->mutex);

        image_buffer_t image;
        object_detect_result_list results;
        memset(&image, 0, sizeof(image));
        memset(&results, 0, sizeof(results));
        image.width = ctx->width;
        image.height = ctx->height;
        image.width_stride = ctx->width;
        image.height_stride = ctx->height;
        image.format = IMAGE_FORMAT_YUV420SP_NV12;
        image.virt_addr = ctx->work_buf;
        image.size = (int)ctx->frame_size;
        image.fd = -1;
        inference_yolo11_model(ctx->rknn_ctx, &image, &results);

        pthread_mutex_lock(&ctx->mutex);
        ctx->latest_results = results;
        ctx->latest_source_seq = seq;
        ctx->has_results = 1;
        ctx->busy = 0;
        ctx->completed_count++;
        int pending_depth = ctx->queue_count;
        pthread_mutex_unlock(&ctx->mutex);

        metrics_row("inference", seq, capture_ts_us,
                    ctx->rknn_ctx->last_preprocess_start_us,
                    ctx->rknn_ctx->last_preprocess_end_us,
                    submit_ts_us,
                    ctx->rknn_ctx->last_infer_start_us,
                    ctx->rknn_ctx->last_infer_end_us,
                    ctx->rknn_ctx->last_postprocess_end_us,
                    0, 0, 0, 0, seq, 1, 0,
                    "async_complete", pending_depth);
        maybe_write_queue_status(ctx, seq, 0);
    }
    return NULL;
}

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
        metrics_row("drop", q->ready.seq, q->ready.capture_ts_us,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    "latest_frame_replaced", 1);
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
    ctx->enabled = 1;

    for (int i = 0; i < BUF_COUNT; i++) {
        if (v4l2->bufs[i].plane[0].dma_fd < 0) {
            ctx->enabled = 0;
            break;
        }
    }

    if (ctx->enabled) {
        APP_LOG("[rga] overlay draw enabled, mode=fd-wrap-helper\n");
    } else {
        APP_LOG("[rga] overlay draw disabled, fallback to CPU draw\n");
    }
}

static void rga_overlay_deinit(rga_overlay_ctx_t *ctx)
{
    (void)ctx;
}

static inline int align_down_even(int v)
{
    return v & ~1;
}

static inline int align_up_even(int v)
{
    return (v + 1) & ~1;
}

static int rga_clip_rect_even(overlay_rect_t *rect, int img_w, int img_h)
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
                         int img_w, int img_h, overlay_rect_t rect, uint8_t y_val)
{
    if (!ctx || !ctx->enabled || buf_index < 0 || buf_index >= BUF_COUNT)
        return -1;
    if (rga_clip_rect_even(&rect, img_w, img_h) < 0)
        return 0;
    if (dma_fd < 0)
        return -1;

    int ret = rga_nv12_fill_rect_fd(dma_fd, img_w, img_h,
                                    rect.x, rect.y, rect.width, rect.height,
                                    y_val);
    if (ret < 0) {
        static int warn_count = 0;
        if (warn_count < 5) {
            APP_LOG("[rga] fill rect failed, fallback to CPU draw\n");
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

    overlay_rect_t top = {x1, y1, x2 - x1 + 1, thickness};
    overlay_rect_t bottom = {x1, y2 - thickness + 1, x2 - x1 + 1, thickness};
    overlay_rect_t left = {x1, y1, thickness, y2 - y1 + 1};
    overlay_rect_t right = {x2 - thickness + 1, y1, thickness, y2 - y1 + 1};

    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, top, y_val) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, bottom, y_val) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, left, y_val) < 0)
        return -1;
    if (rga_fill_rect(ctx, buf_index, dma_fd, img_w, img_h, right, y_val) < 0)
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
#if APP_LOG_ENABLE
            APP_LOG("[ALERT] 鍏ヤ镜! person@(%d,%d,%d,%d) 瓒婅繃y=%d 缃俊搴?%.1f%%\n",
                   x1, y1, x2, y2, BOUNDARY_Y, det->prop * 100.0f);
#endif
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
            APP_LOG("[ALERT] 鍏ヤ镜! person@(%d,%d,%d,%d) 瓒婅繃y=%d 缃俊搴?%.1f%%\n",
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
        metrics_row("capture", q->ready.seq, capture_ts,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "", 1);
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
    ai_async_ctx_t *ai = arg->ai;
    int width = arg->width;
    int height = arg->height;
    int fps = arg->fps;
    const uint64_t ts_base = rtsp_get_reltime();
    uint64_t ts = ts_base;
    const char *h264_dump_path = getenv("PERF_H264_DUMP");
    FILE *h264_dump_fp = (h264_dump_path && h264_dump_path[0]) ?
                         fopen(h264_dump_path, "wb") : NULL;
    int frame_idx = 0;
    uint64_t last_drawn_source_seq = 0;
    latency_stat_t stat;

    memset(&stat, 0, sizeof(stat));

    while (g_running) {
        handoff_frame_t frm;
        if (handoff_get_fresh(q, &frm) < 0)
            continue;

        int64_t t_loop_begin = now_us();
        int64_t handoff_delay = (frm.dqbuf_us > 0 && t_loop_begin >= frm.dqbuf_us) ?
                                t_loop_begin - frm.dqbuf_us : 0;

        int ai_active = ai_is_active();
        int wants_infer = ai_active && (frame_idx % g_infer_interval) == 0;
        int64_t submit_attempt_us = wants_infer ? now_us() : 0;
        int infer_queue_depth = ai_async_queue_depth(ai);
        int submit_result = wants_infer ?
                            ai_async_submit(ai, frm.data, frm.seq,
                                            frm.capture_ts_us, submit_attempt_us,
                                            &infer_queue_depth) : 0;
        int did_infer = submit_result == 1;
        int infer_skipped = wants_infer && submit_result < 0;
        int queue_limit_stop = submit_result == -2;
        int64_t infer_submit_us = did_infer ? submit_attempt_us : 0;
        const char *decision_reason = "interval_not_due";
        if (!g_ai_enabled || g_submit_policy == SUBMIT_DISABLED)
            decision_reason = "ai_disabled";
        else if (!ai_active)
            decision_reason = "ai_deferred_warmup";
        else if (did_infer)
            decision_reason = "async_submit";
        else if (submit_result == -1)
            decision_reason = "npu_busy";
        else if (queue_limit_stop)
            decision_reason = "queue_limit_stop";

        object_detect_result_list od_results;
        uint64_t detection_source_frame_id = 0;
        int has_cached_results = 0;
        ai_async_get_results(ai, &od_results, &detection_source_frame_id,
                             &has_cached_results);
        if (has_cached_results && !g_reuse_boxes &&
            detection_source_frame_id == last_drawn_source_seq) {
            memset(&od_results, 0, sizeof(od_results));
            detection_source_frame_id = 0;
        } else if (has_cached_results) {
            last_drawn_source_seq = detection_source_frame_id;
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
        int64_t overlay_start_us = 0;
        int64_t overlay_end_us = 0;
        int64_t encode_submit_us = 0;
        int64_t t2 = 0;
        uint8_t *h264_data = NULL;
        int h264_size = 0;
        const MppPktSeg *seg = NULL;

        if (use_dmabuf_frame) {
            encode_ptr = (uint8_t *)frm.data;
            encode_buf_index = frm.buf_index;

            int64_t t_draw_begin = now_us();
            overlay_start_us = t_draw_begin;
            if (draw_detections_rga(arg->rga_overlay, frm.buf_index, frm.dma_fd,
                                    width, height, &od_results) < 0) {
                draw_detections_cpu(encode_ptr, width, height, &od_results);
            }
            int64_t t_draw_end = now_us();
            draw_us = t_draw_end - t_draw_begin;
            overlay_end_us = t_draw_end;
            encode_submit_us = now_us();

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
            overlay_start_us = t_draw_begin;
            draw_detections_cpu(encode_ptr, width, height, &od_results);
            int64_t t_draw_end = now_us();
            draw_us = t_draw_end - t_draw_begin;
            overlay_end_us = t_draw_end;
            encode_submit_us = now_us();

            if (mpp_encoder_encode(enc, encode_ptr, width * height * 3 / 2,
                                   -1, &h264_data, &h264_size, &seg) < 0) {
                continue;
            }
            t2 = now_us();
            encode_us = t2 - t_draw_end;
        }

        metrics_row("processed", frm.seq, frm.capture_ts_us,
                    0, 0,
                    infer_submit_us,
                    0, 0, 0,
                    overlay_start_us, overlay_end_us, encode_submit_us, t2,
                    detection_source_frame_id, wants_infer, infer_skipped,
                    decision_reason, infer_queue_depth);
        maybe_write_queue_status(ai, frm.seq, queue_limit_stop);
        if (queue_limit_stop)
            g_running = 0;

        if (h264_size > 0)
            if (h264_dump_fp)
                fwrite(h264_data, 1, (size_t)h264_size, h264_dump_fp);
        if (h264_size > 0)
            rtsp_tx_video(arg->session, h264_data, h264_size, ts);
        int64_t t3 = now_us();

        rtsp_do_event(arg->demo);
        int64_t t_event = now_us();
        frame_idx++;
        ts = ts_base + (uint64_t)frame_idx * 1000000ULL / (uint64_t)fps;

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
                    0, 0, 0, 0,
                    t3 - t2,
                    t_event - t3,
                    t3 - t_loop_begin,
                    capture_ts_to_rtsp,
                    t_event - t_loop_begin,
                    did_infer);

        if (frame_idx % STAT_INTERVAL == 0)
            stat_print_and_reset(&stat, frame_idx);
    }
    if (h264_dump_fp) {
        fflush(h264_dump_fp);
        fclose(h264_dump_fp);
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

    const char *metrics_path = getenv("PERF_METRICS_CSV");
    const char *ai_env = getenv("PERF_AI_ENABLED");
    const char *interval_env = getenv("PERF_INFER_INTERVAL");
    const char *policy_env = getenv("PERF_SUBMIT_POLICY");
    const char *reuse_env = getenv("PERF_REUSE_BOXES");
    const char *trace_env = getenv("PERF_DETAILED_TRACE");
    const char *queue_limit_env = getenv("PERF_INFER_QUEUE_LIMIT");
    g_ai_start_file = getenv("PERF_AI_START_FILE");
    g_queue_status_path = getenv("PERF_QUEUE_STATUS_FILE");
    if (ai_env) g_ai_enabled = atoi(ai_env) != 0;
    if (interval_env && atoi(interval_env) > 0)
        g_infer_interval = atoi(interval_env);
    if (reuse_env) g_reuse_boxes = atoi(reuse_env) != 0;
    if (trace_env) g_detailed_trace = atoi(trace_env) != 0;
    if (queue_limit_env && atoi(queue_limit_env) > 0)
        g_infer_queue_limit = atoi(queue_limit_env);
    if (policy_env) {
        if (strcmp(policy_env, "disabled") == 0)
            g_submit_policy = SUBMIT_DISABLED;
        else if (strcmp(policy_env, "always_queue") == 0)
            g_submit_policy = SUBMIT_ALWAYS_QUEUE;
        else if (strcmp(policy_env, "latest_if_idle") == 0)
            g_submit_policy = SUBMIT_LATEST_IF_IDLE;
        else {
            fprintf(stderr, "invalid PERF_SUBMIT_POLICY: %s\n", policy_env);
            return -1;
        }
    }
    if (!g_ai_enabled)
        g_submit_policy = SUBMIT_DISABLED;
    if (metrics_path && metrics_path[0]) {
        g_metrics_fp = fopen(metrics_path, "w");
        if (!g_metrics_fp) {
            perror("fopen PERF_METRICS_CSV");
            return -1;
        }
        setvbuf(g_metrics_fp, NULL, _IOFBF, 1024 * 1024);
        fprintf(g_metrics_fp, "event,frame_id,capture_ts_us,rga_pre_start_us,rga_pre_end_us,infer_submit_us,infer_start_us,infer_end_us,post_end_us,overlay_start_us,overlay_end_us,encode_submit_us,encode_output_us,detection_source_frame_id,triggered,skipped,skip_reason,infer_queue_depth\n");
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int ret = 0;
    v4l2_ctx_t v4l2 = {0};
    mpp_enc_ctx_t enc = {0};
    rga_overlay_ctx_t rga_overlay;
    rknn_app_context_t rknn_ctx;
    ai_async_ctx_t ai_ctx;
    rtsp_demo_handle demo = NULL;
    rtsp_session_handle session = NULL;
    handoff_queue_t queue;
    pthread_t capture_tid;
    pthread_t process_tid;
    pthread_t ai_tid;
    int v4l2_ok = 0, enc_ok = 0, rknn_ok = 0, queue_ok = 0, ai_ok = 0;
    int capture_started = 0, process_started = 0, ai_started = 0;

    memset(&rknn_ctx, 0, sizeof(rknn_ctx));
    memset(&ai_ctx, 0, sizeof(ai_ctx));
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
            APP_LOG("[main] MPP uses V4L2 DMA-BUF directly, NV12 memcpy disabled\n");
        } else {
            APP_LOG("[main] MPP DMA-BUF import failed, fallback to NV12 memcpy\n");
        }
    }

    init_post_process();
    if (init_yolo11_model(model_path, &rknn_ctx) != 0) {
        fprintf(stderr, "init_yolo11_model failed\n");
        ret = -1; goto cleanup;
    }
    rknn_ok = 1;
    APP_LOG("[main] YOLO11 model loaded: %s\n", model_path);

    if (ai_async_init(&ai_ctx, &rknn_ctx, width, height) != 0) {
        fprintf(stderr, "ai_async_init failed\n");
        ret = -1; goto cleanup;
    }
    ai_ok = 1;

    demo = rtsp_new_demo(8554);
    if (!demo) { ret = -1; goto cleanup; }
    session = rtsp_new_session(demo, "/live");
    if (!session) { ret = -1; goto cleanup; }
    rtsp_set_video(session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(session, rtsp_get_reltime(), rtsp_get_ntptime());

    handoff_queue_init(&queue, &v4l2);
    queue_ok = 1;

    APP_LOG("========================================\n");
    APP_LOG("RTSP: rtsp://YOUR_BOARD_IP:8554/live\n");
    APP_LOG("璁惧: %s  %dx%d@%dfps  瓒婄晫闃堝€? y=%d (涓嶆樉绀烘í绾?\n", dev, width, height, fps, BOUNDARY_Y);
    APP_LOG("澶氱嚎绋嬫ā寮? V4L2 handoff + process/encode/rtsp thread\n");
    APP_LOG("姣?d甯ф墦鍗颁竴娆″欢杩熺粺璁n", STAT_INTERVAL);
    APP_LOG("========================================\n");

    capture_thread_arg_t cap_arg;
    cap_arg.v4l2 = &v4l2;
    cap_arg.queue = &queue;
    process_thread_arg_t proc_arg;
    proc_arg.queue = &queue;
    proc_arg.enc = &enc;
    proc_arg.ai = &ai_ctx;
    proc_arg.demo = demo;
    proc_arg.session = session;
    proc_arg.rga_overlay = &rga_overlay;
    proc_arg.width = width;
    proc_arg.height = height;
    proc_arg.fps = fps;

    if (pthread_create(&ai_tid, NULL, ai_thread_main, &ai_ctx) != 0) {
        fprintf(stderr, "pthread_create ai failed\n");
        ret = -1; goto cleanup;
    }
    ai_started = 1;

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

    APP_LOG("\nStopping...\n");

cleanup:
    g_running = 0;
    if (queue_ok) {
        pthread_mutex_lock(&queue.mutex);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.mutex);
    }
    if (ai_ok) {
        pthread_mutex_lock(&ai_ctx.mutex);
        pthread_cond_broadcast(&ai_ctx.cond);
        pthread_mutex_unlock(&ai_ctx.mutex);
    }
    if (capture_started) pthread_join(capture_tid, NULL);
    if (process_started) pthread_join(process_tid, NULL);
    if (ai_started) pthread_join(ai_tid, NULL);
    if (ai_ok) {
        maybe_write_queue_status(&ai_ctx, queue.latest_seq, 1);
        ai_async_deinit(&ai_ctx);
    }
    deinit_post_process();
    if (rknn_ok) release_yolo11_model(&rknn_ctx);
    rga_overlay_deinit(&rga_overlay);
    if (session) rtsp_del_session(session);
    if (demo) rtsp_del_demo(demo);
    if (enc_ok) mpp_encoder_close(&enc);
    if (queue_ok) handoff_queue_deinit(&queue);
    if (v4l2_ok) v4l2_close(&v4l2);
    if (g_metrics_fp) {
        fflush(g_metrics_fp);
        fclose(g_metrics_fp);
        g_metrics_fp = NULL;
    }

    APP_LOG("Exit.\n");
    return ret;
}
