/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "esp_log.h"
#include "include/board.h"

#include "board_def.h"
#include "i2c_bus.h"

#include "audio_mem.h"
#include "periph_adc_button.h"
#include "i2s_pa.h"
#include "driver/i2c_master.h"

static const char *TAG = "AUDIO_BOARD";

static audio_board_handle_t board_handle = 0;

audio_board_handle_t audio_board_init(void) {
    ESP_LOGI(TAG, "std i2s pa codec init");
    if (board_handle) {
        ESP_LOGW(TAG, "The board has already been initialized!");
        return board_handle;
    }

    board_handle = (audio_board_handle_t) audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
    ESP_LOGI(TAG, "codec init");
    board_handle->audio_hal = audio_board_codec_init();
    ESP_LOGI(TAG, "enable pa");
    return board_handle;
}


audio_hal_handle_t audio_board_adc_init(void) {
    return NULL;
}

audio_hal_handle_t audio_board_codec_init(void) {
    audio_hal_func_t std_i2s_pa_handle = {
        .audio_codec_initialize = i2s_pa_initialize,
        .audio_codec_deinitialize = i2s_pa_deinitialize,
        .audio_codec_ctrl = i2s_pa_ctrl,
        .audio_codec_config_iface = i2s_pa_config_iface,
        .audio_codec_set_mute = i2s_pa_set_mute,
        .audio_codec_set_volume = i2s_pa_set_volume,
        .audio_codec_get_volume = i2s_pa_get_volume,
        .audio_codec_enable_pa = i2s_pa_enable_pa,
        .audio_hal_lock = NULL,
        .handle = NULL,
    };

    audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_codec_cfg.codec_mode = AUDIO_HAL_CODEC_MODE_DECODE;
    audio_hal_handle_t codec_hal = audio_hal_init(&audio_codec_cfg, &std_i2s_pa_handle);
    AUDIO_NULL_CHECK(TAG, codec_hal, return NULL);
    return codec_hal;
}

audio_board_handle_t audio_board_get_handle(void) {
    return board_handle;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board) {
    esp_err_t ret = ESP_OK;
    ret |= audio_hal_deinit(audio_board->audio_hal);
    audio_free(audio_board);
    board_handle = NULL;
    return ret;
}