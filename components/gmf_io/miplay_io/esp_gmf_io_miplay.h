/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include "esp_gmf_io.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    RingbufHandle_t     ringbuf;  /*!< FreeRTOS ring buffer for TS data */
    esp_gmf_io_dir_t    dir;      /*!< IO direction, reader or writer */
    const char         *name;     /*!< Name for this instance */
    esp_gmf_io_cfg_t    io_cfg;   /*!< IO configuration for task and buffer */
} miplay_io_cfg_t;

#define ESP_GMF_IO_MIPLAY_CFG_DEFAULT() {  \
    .ringbuf = NULL,                        \
    .dir     = ESP_GMF_IO_DIR_NONE,         \
    .name    = NULL,                         \
    .io_cfg  = {                             \
        .thread = {                          \
            .stack        = 0,               \
            .prio         = 0,               \
            .core         = 0,               \
            .stack_in_ext = false,           \
        },                                   \
        .buffer_cfg = {                      \
            .io_size     = 0,                \
            .buffer_size = 0,                \
            .read_filter = NULL,             \
        },                                   \
        .enable_speed_monitor = false,       \
    },                                       \
}

esp_gmf_err_t esp_gmf_io_miplay_init(miplay_io_cfg_t *config, esp_gmf_io_handle_t *io);

esp_gmf_err_t esp_gmf_io_miplay_set_ringbuf(esp_gmf_io_handle_t io, RingbufHandle_t ringbuf);

#ifdef __cplusplus
}
#endif
