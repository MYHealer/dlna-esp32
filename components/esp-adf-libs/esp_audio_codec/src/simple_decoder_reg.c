/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 *
 * NOTE: 移除了 CONFIG_AUDIO_SIMPLE_DEC_* 的 #ifdef 守卫。
 * 原因同 audio_decoder_reg.c：Kconfig 不被 IDF 发现，强制注册全部。
 */

#include "sdkconfig.h"
#include "esp_audio_simple_dec_default.h"

esp_audio_err_t esp_audio_simple_dec_register_default(void)
{
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    ret |= esp_wav_dec_register();
    ret |= esp_m4a_dec_register();
    ret |= esp_ts_dec_register();
    ret |= esp_ogg_dec_register();
    return ret;
}

void esp_audio_simple_dec_unregister_default(void)
{
    esp_wav_dec_unregister();
    esp_m4a_dec_unregister();
    esp_ts_dec_unregister();
    esp_ogg_dec_unregister();
}