/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 *
 * NOTE: 移除了 CONFIG_AUDIO_DECODER_XXX_SUPPORT 的 #ifdef 守卫。
 * 原因：esp_audio_codec 是嵌套在 esp-adf-libs 下的组件，其 Kconfig 不被
 * ESP-IDF 自动发现，所有 CONFIG_AUDIO_DECODER_* 宏都未定义，导致
 * register_default 变成空操作。这里强制注册全部解码器。
 */

#include "sdkconfig.h"
#include "esp_audio_dec_default.h"

esp_audio_err_t esp_audio_dec_register_default(void)
{
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    ret |= esp_mp3_dec_register();
    ret |= esp_aac_dec_register();
    ret |= esp_g711a_dec_register();
    ret |= esp_g711u_dec_register();
    ret |= esp_amrnb_dec_register();
    ret |= esp_amrwb_dec_register();
    ret |= esp_flac_dec_register();
    ret |= esp_vorbis_dec_register();
    ret |= esp_opus_dec_register();
    ret |= esp_adpcm_dec_register();
    ret |= esp_alac_dec_register();
    ret |= esp_pcm_dec_register();
    ret |= esp_sbc_dec_register();
    ret |= esp_lc3_dec_register();
    ret |= esp_g722_dec_register();
    return ret;
}

void esp_audio_dec_unregister_default(void)
{
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AAC);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G711A);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G711U);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AMRNB);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AMRWB);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_FLAC);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_VORBIS);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_ADPCM);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_ALAC);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_PCM);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_SBC);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_LC3);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G722);
}