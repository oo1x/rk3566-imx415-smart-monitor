// Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "yolo11.h"
#include <time.h>
#include "common.h"
#include "file_utils.h"
#include "image_utils.h"

#ifndef RKNN_YOLO_LOG_ENABLE
#define RKNN_YOLO_LOG_ENABLE 0
#endif

#if RKNN_YOLO_LOG_ENABLE
#define RKNN_YOLO_LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define RKNN_YOLO_LOG(...) do { } while (0)
#endif

static void dump_tensor_attr(rknn_tensor_attr *attr) {
    char dims[128] = {0};
    for (int i = 0; i < attr->n_dims; ++i) {
        int idx = strlen(dims);
        sprintf(&dims[idx], "%d%s", attr->dims[i], (i == attr->n_dims - 1) ? "" : ", ");
    }
    RKNN_YOLO_LOG("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, w_stride = %d, size_with_stride = %d, "
           "fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, dims, attr->n_elems, attr->size, attr->w_stride, attr->size_with_stride,
           get_format_string(attr->fmt), get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp,
           attr->scale);
}

int init_yolo11_model(const char *model_path, rknn_app_context_t *app_ctx) {
    int ret;
    int model_len = 0;
    char *model;
    rknn_context ctx = 0;

    // Load RKNN Model
    model_len = read_data_from_file(model_path, &model);
    if (model == NULL) {
        RKNN_YOLO_LOG("load_model fail!\n");
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        RKNN_YOLO_LOG("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        RKNN_YOLO_LOG("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    RKNN_YOLO_LOG("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // Get Model Input Info
    RKNN_YOLO_LOG("input tensors:\n");
    rknn_tensor_attr input_native_attrs[io_num.n_input];
    memset(input_native_attrs, 0, sizeof(input_native_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_native_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &(input_native_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            RKNN_YOLO_LOG("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_native_attrs[i]));
    }

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_native_attrs[0].type = RKNN_TENSOR_UINT8;
    app_ctx->input_mems[0] = rknn_create_mem(ctx, input_native_attrs[0].size_with_stride);

    // Set input tensor memory
    ret = rknn_set_io_mem(ctx, app_ctx->input_mems[0], &input_native_attrs[0]);
    if (ret < 0) {
        RKNN_YOLO_LOG("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Output Info
    RKNN_YOLO_LOG("output tensors:\n");
    rknn_tensor_attr output_native_attrs[io_num.n_output];
    memset(output_native_attrs, 0, sizeof(output_native_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_native_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &(output_native_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            RKNN_YOLO_LOG("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_native_attrs[i]));
    }

    // Set output tensor memory
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        app_ctx->output_mems[i] = rknn_create_mem(ctx, output_native_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(ctx, app_ctx->output_mems[i], &output_native_attrs[i]);
        if (ret < 0) {
            RKNN_YOLO_LOG("output_mems rknn_set_io_mem fail! ret=%d\n", ret);
            return -1;
        }
    }

    // Set to context
    app_ctx->rknn_ctx = ctx;

    // TODO
    if (output_native_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && output_native_attrs[0].type == RKNN_TENSOR_INT8) {
        app_ctx->is_quant = true;
    } else {
        app_ctx->is_quant = false;
    }

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            RKNN_YOLO_LOG("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            RKNN_YOLO_LOG("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
    }

    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    app_ctx->input_native_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_native_attrs, input_native_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_native_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_native_attrs, output_native_attrs, io_num.n_output * sizeof(rknn_tensor_attr));


    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        RKNN_YOLO_LOG("model is NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    } else {
        RKNN_YOLO_LOG("model is NHWC input fmt\n");
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    RKNN_YOLO_LOG("model input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);

    return 0;
}

int release_yolo11_model(rknn_app_context_t *app_ctx) {
    int ret;
    if (app_ctx->input_attrs != NULL) {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL) {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->input_native_attrs != NULL) {
        free(app_ctx->input_native_attrs);
        app_ctx->input_native_attrs = NULL;
    }
    if (app_ctx->output_native_attrs != NULL) {
        free(app_ctx->output_native_attrs);
        app_ctx->output_native_attrs = NULL;
    }

    for (int i = 0; i < app_ctx->io_num.n_input; i++) {
        if (app_ctx->input_mems[i] != NULL) {
            ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->input_mems[i]);
            if (ret != RKNN_SUCC) {
                RKNN_YOLO_LOG("rknn_destroy_mem fail! ret=%d\n", ret);
                return -1;
            }
        }
    }
    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        if (app_ctx->output_mems[i] != NULL) {
            ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->output_mems[i]);
            if (ret != RKNN_SUCC) {
                RKNN_YOLO_LOG("rknn_destroy_mem fail! ret=%d\n", ret);
                return -1;
            }
        }
    }
    if (app_ctx->rknn_ctx != 0) {
        ret = rknn_destroy(app_ctx->rknn_ctx);
        if (ret != RKNN_SUCC) {
            RKNN_YOLO_LOG("rknn_destroy fail! ret=%d\n", ret);
            return -1;
        }
        app_ctx->rknn_ctx = 0;

    }
    return 0;
}

static int64_t yolo_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int inference_yolo11_model(rknn_app_context_t *app_ctx, image_buffer_t *img, object_detect_result_list *od_results) {
    int ret;
    int64_t t0, t_pre, t_run, t_out, t_post;
    image_buffer_t dst_img;
    letterbox_t letter_box;
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    int bg_color = 114;

    if ((!app_ctx) || !(img) || (!od_results)) {
        return -1;
    }

    app_ctx->last_preprocess_us = 0;
    app_ctx->last_rknn_run_us = 0;
    app_ctx->last_output_convert_us = 0;
    app_ctx->last_postprocess_us = 0;
    app_ctx->last_preprocess_start_us = 0;
    app_ctx->last_preprocess_end_us = 0;
    app_ctx->last_infer_start_us = 0;
    app_ctx->last_infer_end_us = 0;
    app_ctx->last_postprocess_end_us = 0;
    t0 = yolo_now_us();
    app_ctx->last_preprocess_start_us = t0;

    memset(od_results, 0x00, sizeof(*od_results));
    memset(&letter_box, 0, sizeof(letterbox_t));
    memset(&dst_img, 0, sizeof(image_buffer_t));

    // Pre Process
    dst_img.width = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size = get_image_size(&dst_img);
    dst_img.fd = app_ctx->input_mems[0]->fd;
    dst_img.virt_addr = (unsigned char*)app_ctx->input_mems[0]->virt_addr;

    if (dst_img.virt_addr == NULL && dst_img.fd == 0) {
        RKNN_YOLO_LOG("malloc buffer size:%d fail!\n", dst_img.size);
        return -1;
    }

    // letterbox
    ret = convert_image_with_letterbox(img, &dst_img, &letter_box, bg_color);
    t_pre = yolo_now_us();
    app_ctx->last_preprocess_us = t_pre - t0;
    app_ctx->last_preprocess_end_us = t_pre;
    if (ret < 0) {
        RKNN_YOLO_LOG("convert_image_with_letterbox fail! ret=%d\n", ret);
        return -1;
    }

    // Run
    app_ctx->last_infer_start_us = yolo_now_us();
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    t_run = yolo_now_us();
    app_ctx->last_infer_end_us = t_run;
    app_ctx->last_rknn_run_us = t_run - t_pre;
    if (ret < 0) {
        RKNN_YOLO_LOG("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    t_out = yolo_now_us();
    app_ctx->last_output_convert_us = 0;

    // Post Process directly reads native zero-copy output buffers.
    ret = post_process_native(app_ctx, &letter_box, box_conf_threshold, nms_threshold, od_results);
    t_post = yolo_now_us();
    app_ctx->last_postprocess_us = t_post - t_out;
    app_ctx->last_postprocess_end_us = t_post;

    return ret;
}
