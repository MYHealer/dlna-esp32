/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "esp_gmf_io_miplay.h"
#include "esp_gmf_oal_mem.h"
#include "esp_log.h"

typedef struct {
    esp_gmf_io_t   base;
    bool           is_open;
    RingbufHandle_t ringbuf;
} miplay_io_stream_t;

static const char *TAG = "ESP_GMF_MIPLAY";

static esp_gmf_err_t esp_gmf_io_miplay_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return esp_gmf_io_miplay_init(cfg, io);
}

static esp_gmf_err_t _miplay_open(esp_gmf_io_handle_t io)
{
    miplay_io_stream_t *self = (miplay_io_stream_t *)io;
    self->is_open = true;
    ESP_LOGI(TAG, "Open, ringbuf=%p", self->ringbuf);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_io_t _miplay_acquire_read(esp_gmf_io_handle_t handle, void *payload,
                                              uint32_t wanted_size, int block_ticks)
{
    miplay_io_stream_t *self = (miplay_io_stream_t *)handle;
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;

    if (wanted_size == 0) {
        pload->valid_size = 0;
        return ESP_GMF_IO_OK;
    }
    if (self->ringbuf == NULL) {
        return ESP_GMF_IO_FAIL;
    }

    size_t item_size = 0;
    void *data = xRingbufferReceiveUpTo(self->ringbuf, &item_size,
                                         pdMS_TO_TICKS(1000), wanted_size);
    if (data == NULL || item_size == 0) {
        pload->valid_size = 0;
        return ESP_GMF_IO_OK;
    }

    memcpy(pload->buf, data, item_size);
    vRingbufferReturnItem(self->ringbuf, data);
    pload->valid_size = item_size;
    ESP_LOGD(TAG, "Read %d bytes", item_size);
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _miplay_release_read(esp_gmf_io_handle_t handle, void *payload,
                                              int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _miplay_acquire_write(esp_gmf_io_handle_t handle, void *payload,
                                               uint32_t wanted_size, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _miplay_release_write(esp_gmf_io_handle_t handle, void *payload,
                                               int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_t _miplay_seek(esp_gmf_io_handle_t io, uint64_t seek_byte_pos)
{
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _miplay_close(esp_gmf_io_handle_t io)
{
    miplay_io_stream_t *self = (miplay_io_stream_t *)io;
    if (self->is_open) {
        self->is_open = false;
    }
    ESP_LOGI(TAG, "Close");
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _miplay_delete(esp_gmf_io_handle_t io)
{
    if (io != NULL) {
        miplay_io_stream_t *self = (miplay_io_stream_t *)io;
        esp_gmf_oal_free(OBJ_GET_CFG(self));
        esp_gmf_io_deinit(io);
        esp_gmf_oal_free(self);
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t esp_gmf_io_miplay_init(miplay_io_cfg_t *config, esp_gmf_io_handle_t *io)
{
    ESP_GMF_NULL_CHECK(TAG, config, return ESP_GMF_ERR_INVALID_ARG;);
    ESP_GMF_NULL_CHECK(TAG, io, return ESP_GMF_ERR_INVALID_ARG;);
    *io = NULL;
    esp_gmf_err_t ret = ESP_GMF_ERR_OK;
    miplay_io_stream_t *self = esp_gmf_oal_calloc(1, sizeof(miplay_io_stream_t));
    ESP_GMF_MEM_VERIFY(TAG, self, return ESP_GMF_ERR_MEMORY_LACK,
                       "miplay io", sizeof(miplay_io_stream_t));
    self->base.dir = config->dir;
    self->base.type = ESP_GMF_IO_TYPE_BYTE;
    self->ringbuf = config->ringbuf;

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)self;
    obj->new_obj = esp_gmf_io_miplay_new;
    obj->del_obj = _miplay_delete;

    miplay_io_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(*config));
    ESP_GMF_MEM_VERIFY(TAG, cfg, {ret = ESP_GMF_ERR_MEMORY_LACK; goto _miplay_fail;},
                       "miplay io cfg", sizeof(*config));
    memcpy(cfg, config, sizeof(*config));
    esp_gmf_obj_set_config(obj, cfg, sizeof(*config));
    ret = esp_gmf_obj_set_tag(obj, (config->name == NULL ? "io_miplay" : config->name));
    ESP_GMF_RET_ON_NOT_OK(TAG, ret, goto _miplay_fail, "Failed to set tag");

    self->base.close = _miplay_close;
    self->base.open = _miplay_open;
    self->base.seek = _miplay_seek;
    self->base.reset = NULL;
    if (self->base.dir == ESP_GMF_IO_DIR_READER) {
        self->base.acquire_read = _miplay_acquire_read;
        self->base.release_read = _miplay_release_read;
    } else {
        self->base.acquire_write = _miplay_acquire_write;
        self->base.release_write = _miplay_release_write;
    }

    esp_gmf_io_cfg_t io_cfg = {
        .thread = {
            .stack = config->io_cfg.thread.stack,
            .prio = config->io_cfg.thread.prio,
            .core = config->io_cfg.thread.core,
            .stack_in_ext = config->io_cfg.thread.stack_in_ext,
        },
        .buffer_cfg = {
            .io_size = config->io_cfg.buffer_cfg.io_size,
            .buffer_size = config->io_cfg.buffer_cfg.buffer_size,
        },
        .enable_speed_monitor = config->io_cfg.enable_speed_monitor,
    };
    ret = esp_gmf_io_init(obj, &io_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        goto _miplay_fail;
    }
    *io = obj;
    ESP_LOGD(TAG, "Init, %s-%p", OBJ_GET_TAG(obj), self);
    return ESP_GMF_ERR_OK;
_miplay_fail:
    esp_gmf_obj_delete(obj);
    return ret;
}

esp_gmf_err_t esp_gmf_io_miplay_set_ringbuf(esp_gmf_io_handle_t io, RingbufHandle_t ringbuf)
{
    miplay_io_stream_t *self = (miplay_io_stream_t *)io;
    ESP_GMF_NULL_CHECK(TAG, self, return ESP_GMF_ERR_INVALID_ARG;);
    self->ringbuf = ringbuf;
    return ESP_GMF_ERR_OK;
}
