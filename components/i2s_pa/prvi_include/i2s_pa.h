//
// Created by lustre on 2026/4/13.
//

#ifndef STD_I2S_DLNA_STD_I2S_PA_CODEC_H
#define STD_I2S_DLNA_STD_I2S_PA_CODEC_H
#include "driver/i2s_std.h"

esp_err_t i2s_pa_initialize(audio_hal_codec_config_t *codec_cfg);

esp_err_t i2s_pa_deinitialize(void);
esp_err_t i2s_pa_ctrl(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state);
esp_err_t i2s_pa_config_iface(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface);
esp_err_t i2s_pa_set_mute(bool mute);
esp_err_t i2s_pa_set_volume(int volume);
esp_err_t i2s_pa_get_volume(int *volume);
esp_err_t i2s_pa_enable_pa(bool enable);

#endif // STD_I2S_DLNA_STD_I2S_PA_CODEC_H