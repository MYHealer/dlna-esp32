//
// Created by lustre on 2026/4/13.
//
#include "audio_hal.h"
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2s_pa.h"

static const char *TAG = "i2s_pa";

typedef struct {
    int volume;
    bool muted;
    bool initialized;
    audio_hal_iface_bits_t bits;
} i2s_pa_context_t;

static i2s_pa_context_t std_i2s_pa_ctx = {
    .volume = AUDIO_HAL_VOL_DEFAULT,
    .muted = false,
    .initialized = false,
    .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
};

esp_err_t i2s_pa_initialize(audio_hal_codec_config_t *codec_cfg) {
    ESP_LOGI(TAG, "std_i2s_pa initialize");
    std_i2s_pa_ctx.bits = codec_cfg->i2s_iface.bits;
    if (std_i2s_pa_ctx.initialized) {
        ESP_LOGW(TAG, "std_i2s_pa already initialized");
        return ESP_OK;
    }
    std_i2s_pa_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t i2s_pa_deinitialize(void) {
    ESP_LOGI(TAG, "std_i2s_pa deinitialize");
    std_i2s_pa_ctx.initialized = false;
    std_i2s_pa_ctx.volume = AUDIO_HAL_VOL_DEFAULT;
    std_i2s_pa_ctx.muted = false;
    return ESP_OK;
}

esp_err_t i2s_pa_ctrl(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state) {
    ESP_LOGI(TAG, "std_i2s_pa ctrl - mode: %d, state: %d", mode, ctrl_state);

    if (!std_i2s_pa_ctx.initialized) {
        ESP_LOGW(TAG, "std_i2s_pa not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctrl_state == AUDIO_HAL_CTRL_START) {
        ESP_LOGI(TAG, "std_i2s_pa start");
    } else if (ctrl_state == AUDIO_HAL_CTRL_STOP) {
        ESP_LOGI(TAG, "std_i2s_pa stop");
    }

    return ESP_OK;
}

esp_err_t i2s_pa_config_iface(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface) {
    ESP_LOGI(TAG, "std_i2s_pa config iface - mode: %d", mode);
    std_i2s_pa_ctx.bits = iface->bits;
    if (!std_i2s_pa_ctx.initialized) {
        ESP_LOGW(TAG, "std_i2s_pa not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t i2s_pa_set_mute(bool mute) {
    ESP_LOGI(TAG, "std_i2s_pa set mute: %d", mute);
    std_i2s_pa_ctx.muted = mute;
    return ESP_OK;
}

esp_err_t i2s_pa_set_volume(int volume) {
    ESP_LOGI(TAG, "std_i2s_pa set volume: %d", volume);

    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    std_i2s_pa_ctx.volume = volume;

    if (volume == 0) {
        std_i2s_pa_ctx.muted = true;
        i2s_pa_set_mute(true);
    } else {
        std_i2s_pa_ctx.muted = false;
        i2s_pa_set_mute(false);
    }

    return ESP_OK;
}

esp_err_t i2s_pa_get_volume(int *volume) {
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *volume = std_i2s_pa_ctx.volume;
    return ESP_OK;
}

esp_err_t i2s_pa_enable_pa(bool enable) {
    ESP_LOGI(TAG, "std_i2s_pa enable PA: %d", enable);
    return ESP_OK;
}