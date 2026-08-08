/*
 * I2S PA board — esp_codec_dev 初始化 (PCM5102A, 无 I2C 控制)
 *
 * 替换原 ADF audio_hal/audio_board，使用 esp_codec_dev 直接管理 I2S。
 * PCM5102A 无控制接口，codec_if=NULL，音量由 GMF ALC 软件控制。
 */

#ifndef _AUDIO_BOARD_H_
#define _AUDIO_BOARD_H_

#include "board_def.h"
/* 仅保留 esp_codec_dev.h 用于 esp_codec_dev_handle_t 类型 */
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取 I2S TX 通道句柄（绕过 esp_codec_dev 直接写 I2S 时使用）
 *
 * @return i2s_chan_handle_t，未初始化返回 NULL
 */
i2s_chan_handle_t audio_out_get_tx_handle(void);

/**
 * @brief 初始化 I2S + esp_codec_dev（PCM5102A 纯 I2S 输出）
 *
 * 内部创建 I2S STD TX 通道 + codec_dev handle。
 * 返回的 handle 可直接传给 esp_gmf_io_codec_dev_set_dev()。
 *
 * @return esp_codec_dev_handle_t，失败返回 NULL
 */
esp_codec_dev_handle_t audio_out_init(void);

/**
 * @brief 更新输出采样率（动态跟随源采样率时调用）
 *
 * 内部调用 esp_codec_dev_open() 重新配置 I2S 格式参数。
 *
 * @param dev    codec_dev handle
 * @param rate   新采样率 (Hz)
 * @param ch     通道数 (1 or 2)
 * @param bits   位深 (16/24/32)
 */
void audio_out_set_clk(esp_codec_dev_handle_t dev, int rate, int ch, int bits);

#ifdef __cplusplus
}
#endif

#endif
