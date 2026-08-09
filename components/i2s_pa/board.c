/*
 * I2S PA board — 裸 I2S 输出 (PCM5102A)
 *
 * 绕过 esp_codec_dev（其软件音量默认 0 静音）。
 * 直接初始化 I2S 并暴露 tx_handle，音量由 GMF ALC 软件控制。
 */

#include "esp_log.h"
#include "esp_err.h"
#include "include/board.h"

#include "driver/i2s_std.h"

static const char *TAG = "AUDIO_OUT";

static i2s_chan_handle_t s_tx_handle = NULL;
static bool s_initialized = false;

i2s_chan_handle_t audio_out_get_tx_handle(void)
{
    return s_tx_handle;
}

esp_codec_dev_handle_t audio_out_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return (esp_codec_dev_handle_t)s_tx_handle;
    }

    /* ── 1. 创建 I2S TX 通道 ── */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    /* ── 2. 初始化 I2S STD 模式 ── */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_I2S_MCLK,
            .bclk = GPIO_I2S_SCLK,
            .ws   = GPIO_I2S_LRCK,
            .dout = GPIO_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    i2s_channel_enable(s_tx_handle);
    s_initialized = true;
    ESP_LOGI(TAG, "I2S STD TX enabled (MCLK=GPIO%d, BCK=GPIO%d, LRCK=GPIO%d, DOUT=GPIO%d)",
             GPIO_I2S_MCLK, GPIO_I2S_SCLK, GPIO_I2S_LRCK, GPIO_I2S_DOUT);

    /* 返回 dummy 非空指针，供 esp_gmf_io_codec_dev_set_dev 的 NULL 检查通过 */
    return (esp_codec_dev_handle_t)s_tx_handle;
}

void audio_out_set_clk(esp_codec_dev_handle_t dev, int rate, int ch, int bits)
{
    (void)dev;
    if (!s_tx_handle || !s_initialized) {
        ESP_LOGW(TAG, "I2S not ready, skip clk reconfig");
        return;
    }

    /* 禁用通道 → 重配时钟/时隙 → 重新启用（原子操作，参考 FAKE_POD_NANO） */
    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
        return;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    /* 24-bit 需要 MCLK 384x 倍频，否则 BCLK 分频不整数导致采样率不精确 */
    if (bits == 24) {
        clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    }
    ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reconfig_std_clock(%d) failed: %s", rate, esp_err_to_name(ret));
    }

    i2s_data_bit_width_t bit_w;
    switch (bits) {
        case 24: bit_w = I2S_DATA_BIT_WIDTH_24BIT; break;
        case 32: bit_w = I2S_DATA_BIT_WIDTH_32BIT; break;
        default: bit_w = I2S_DATA_BIT_WIDTH_16BIT; break;
    }
    i2s_slot_mode_t slot_mode = (ch <= 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_w, slot_mode);
    ret = i2s_channel_reconfig_std_slot(s_tx_handle, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reconfig_std_slot failed: %s", esp_err_to_name(ret));
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "I2S reconfig: %d Hz, %d bit, %d ch", rate, bits, ch);
}
