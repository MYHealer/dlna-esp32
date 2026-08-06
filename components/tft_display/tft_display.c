/*
 * ST7735S 1.8" TFT 显示驱动 — ESP-IDF SPI Master 实现
 * 128×160 RGB565, DMA 传输, 内置5×8 ASCII 字体
 */

#include "tft_display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TFT";

/* ── SPI 设备句柄 ── */
static spi_device_handle_t s_spi;

/* ── 5×8 ASCII 字体 (32-126) ── */
static const uint8_t font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 (space) */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x00,0x08,0x14,0x22,0x41}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x41,0x22,0x14,0x08,0x00}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x01,0x01}, /* 70 F */
    {0x3E,0x41,0x41,0x51,0x32}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x03,0x04,0x78,0x04,0x03}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* 91 [ */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* 93 ] */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 _ */
    {0x00,0x01,0x02,0x04,0x00}, /* 96 ` */
    {0x20,0x54,0x54,0x54,0x78}, /* 97 a */
    {0x7F,0x48,0x44,0x44,0x38}, /* 98 b */
    {0x38,0x44,0x44,0x44,0x20}, /* 99 c */
    {0x38,0x44,0x44,0x48,0x7F}, /* 100 d */
    {0x38,0x54,0x54,0x54,0x18}, /* 101 e */
    {0x08,0x7E,0x09,0x01,0x02}, /* 102 f */
    {0x08,0x14,0x54,0x54,0x3C}, /* 103 g */
    {0x7F,0x08,0x04,0x04,0x78}, /* 104 h */
    {0x00,0x44,0x7D,0x40,0x00}, /* 105 i */
    {0x20,0x40,0x44,0x3D,0x00}, /* 106 j */
    {0x00,0x7F,0x10,0x28,0x44}, /* 107 k */
    {0x00,0x41,0x7F,0x40,0x00}, /* 108 l */
    {0x7C,0x04,0x18,0x04,0x78}, /* 109 m */
    {0x7C,0x08,0x04,0x04,0x78}, /* 110 n */
    {0x38,0x44,0x44,0x44,0x38}, /* 111 o */
    {0x7C,0x14,0x14,0x14,0x08}, /* 112 p */
    {0x08,0x14,0x14,0x18,0x7C}, /* 113 q */
    {0x7C,0x08,0x04,0x04,0x08}, /* 114 r */
    {0x48,0x54,0x54,0x54,0x20}, /* 115 s */
    {0x04,0x3F,0x44,0x40,0x20}, /* 116 t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 117 u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 118 v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 119 w */
    {0x44,0x28,0x10,0x28,0x44}, /* 120 x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 121 y */
    {0x44,0x64,0x54,0x4C,0x44}, /* 122 z */
    {0x00,0x08,0x36,0x41,0x00}, /* 123 { */
    {0x00,0x00,0x7F,0x00,0x00}, /* 124 | */
    {0x00,0x41,0x36,0x08,0x00}, /* 125 } */
    {0x08,0x08,0x2A,0x1C,0x08}, /* 126 ~ */
};

/* ── ST7735S 命令 ── */
#define ST7735_NOP      0x00
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_INVON    0x21
#define ST7735_DISPOFF  0x28
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

/* MADCTL 位 */
#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_ML       0x10
#define MADCTL_RGB      0x00
#define MADCTL_BGR      0x08

/* ── SPI 发送辅助 ── */
static void tft_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_PIN_DC, 0);  /* 命令模式 */
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_data(const uint8_t *data, int len)
{
    if (len == 0) return;
    gpio_set_level(TFT_PIN_DC, 1);  /* 数据模式 */
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
    tft_cmd(cmd);
    tft_data(data, len);
}

/* 设置绘图窗口 */
static void tft_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t caset[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    tft_cmd_data(ST7735_CASET, caset, 4);
    tft_cmd_data(ST7735_RASET, raset, 4);
}

/* ── 初始化序列 ── */
static void tft_init_sequence(void)
{
    /* 硬件复位 */
    gpio_set_level(TFT_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    tft_cmd(ST7735_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    tft_cmd(ST7735_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 帧率控制 */
    uint8_t frmctr1[] = {0x01, 0x2C, 0x2D};
    tft_cmd_data(ST7735_FRMCTR1, frmctr1, 3);
    uint8_t frmctr2[] = {0x01, 0x2C, 0x2D};
    tft_cmd_data(ST7735_FRMCTR2, frmctr2, 3);
    uint8_t frmctr3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
    tft_cmd_data(ST7735_FRMCTR3, frmctr3, 6);

    uint8_t invctr[] = {0x07};
    tft_cmd_data(ST7735_INVCTR, invctr, 1);

    /* 电源控制 */
    uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    tft_cmd_data(ST7735_PWCTR1, pwctr1, 3);
    uint8_t pwctr2[] = {0xC5};
    tft_cmd_data(ST7735_PWCTR2, pwctr2, 1);
    uint8_t pwctr3[] = {0x0A, 0x00};
    tft_cmd_data(ST7735_PWCTR3, pwctr3, 2);
    uint8_t pwctr4[] = {0x8A, 0x2A};
    tft_cmd_data(ST7735_PWCTR4, pwctr4, 2);
    uint8_t pwctr5[] = {0x8A, 0xEE};
    tft_cmd_data(ST7735_PWCTR5, pwctr5, 2);

    uint8_t vmctr1[] = {0x0E};
    tft_cmd_data(ST7735_VMCTR1, vmctr1, 1);

    tft_cmd(ST7735_INVOFF);

    /* 颜色模式: 16bit/pixel (RGB565) */
    uint8_t colmod[] = {0x05};
    tft_cmd_data(ST7735_COLMOD, colmod, 1);

    /* MADCTL: 竖屏 180°翻转, BGR */
    uint8_t madctl[] = {MADCTL_MX | MADCTL_MY | MADCTL_BGR};
    tft_cmd_data(ST7735_MADCTL, madctl, 1);

    /* Gamma 校正 */
    uint8_t gmctrp1[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                         0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    tft_cmd_data(ST7735_GMCTRP1, gmctrp1, 16);
    uint8_t gmctrn1[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                         0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
    tft_cmd_data(ST7735_GMCTRN1, gmctrn1, 16);

    tft_cmd(ST7735_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));

    tft_cmd(ST7735_DISPON);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ── 公共 API ── */

void tft_write_rect(int x, int y, int w, int h, const uint16_t *data)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || !data) return;
    if (x + w > TFT_W) w = TFT_W - x;
    if (y + h > TFT_H) h = TFT_H - y;

    tft_set_window(x, y, x + w - 1, y + h - 1);
    tft_cmd(ST7735_RAMWR);
    gpio_set_level(TFT_PIN_DC, 1);

    spi_transaction_t t = {
        .length = w * h * 16,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

esp_err_t tft_init(void)
{
    ESP_LOGI(TAG, "Init ST7735S: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
             TFT_PIN_SCLK, TFT_PIN_MOSI, TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_RST, TFT_PIN_BL);

    /* 配置 DC 和 RST 为 GPIO 输出 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TFT_PIN_DC) | (1ULL << TFT_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 配置背光 GPIO */
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << TFT_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_conf);
    gpio_set_level(TFT_PIN_BL, 1);  /* 默认开背光 */

    /* 初始化 SPI */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = TFT_PIN_SCLK,
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_W * TFT_H * 2,
    };
    esp_err_t ret = spi_bus_initialize(TFT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = TFT_SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = TFT_PIN_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(TFT_SPI_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    tft_init_sequence();

    /* 清屏 */
    tft_fill_screen(COLOR_BLACK);

    ESP_LOGI(TAG, "ST7735S ready (%dx%d landscape)", TFT_W, TFT_H);
    return ESP_OK;
}

void tft_set_backlight(uint8_t brightness)
{
    gpio_set_level(TFT_PIN_BL, brightness > 0 ? 1 : 0);
}

void tft_fill_screen(uint16_t color)
{
    tft_fill_rect(0, 0, TFT_W, TFT_H, color);
}

void tft_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > TFT_W) w = TFT_W - x;
    if (y + h > TFT_H) h = TFT_H - y;

    tft_set_window(x, y, x + w - 1, y + h - 1);
    tft_cmd(ST7735_RAMWR);
    gpio_set_level(TFT_PIN_DC, 1);

    /* 发送行数据，每行独立构造 */
    uint8_t line_buf[TFT_W * 2];
    int send_w = (w > TFT_W) ? TFT_W : w;
    for (int i = 0; i < send_w; i++) {
        line_buf[i * 2] = color >> 8;
        line_buf[i * 2 + 1] = color & 0xFF;
    }

    for (int row = 0; row < h; row++) {
        spi_transaction_t t = {
            .length = send_w * 16,
            .tx_buffer = line_buf,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
}

void tft_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= TFT_W || y >= TFT_H) return;
    tft_set_window(x, y, x, y);
    uint8_t buf[2] = {color >> 8, color & 0xFF};
    tft_cmd_data(ST7735_RAMWR, buf, 2);
}

void tft_draw_hline(int x, int y, int w, uint16_t color)
{
    tft_fill_rect(x, y, w, 1, color);
}

void tft_draw_vline(int x, int y, int h, uint16_t color)
{
    tft_fill_rect(x, y, 1, h, color);
}

void tft_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    tft_draw_hline(x, y, w, color);
    tft_draw_hline(x, y + h - 1, w, color);
    tft_draw_vline(x, y, h, color);
    tft_draw_vline(x + w - 1, y, h, color);
}

int tft_draw_char(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font5x8[ch - 32];

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 8; row++) {
            uint16_t c = (line & 0x01) ? color : bg;
            if (size == 1) {
                tft_draw_pixel(x + col, y + row, c);
            } else {
                tft_fill_rect(x + col * size, y + row * size, size, size, c);
            }
            line >>= 1;
        }
    }
    /* 字符间距1像素 */
    if (bg != color) {
        for (int row = 0; row < 8 * size; row++) {
            tft_draw_pixel(x + 5 * size, y + row, bg);
        }
    }
    return 6 * size;
}

void tft_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    while (*str) {
        x += tft_draw_char(x, y, *str, color, bg, size);
        str++;
    }
}

void tft_draw_string_truncate(int x, int y, const char *str, int max_width,
                               uint16_t color, uint16_t bg, uint8_t size)
{
    int cx = x;
    while (*str) {
        int char_w = 6 * size;
        if (cx + char_w - x > max_width) break;
        cx += tft_draw_char(cx, y, *str, color, bg, size);
        str++;
    }
    /* 填充剩余区域 */
    if (cx - x < max_width) {
        tft_fill_rect(cx, y, max_width - (cx - x), 8 * size, bg);
    }
}

/* ══════════════════════════════════════════════
 *  Hermes 风格 UI 组件
 * ══════════════════════════════════════════════ */

/* 粗体字符：偏移1px重绘（Hermes embolden 技巧） */
int tft_draw_char_bold(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size)
{
    tft_draw_char(x, y, ch, color, bg, size);
    tft_draw_char(x + 1, y, ch, color, bg, size);
    return 6 * size + 1;
}

void tft_draw_string_bold(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    while (*str) {
        x += tft_draw_char_bold(x, y, *str, color, bg, size);
        str++;
    }
}

/* Hermes 进度条：细线 + 圆点指示器 */
void tft_draw_progress_bar(int x, int y, int w, int permille)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    /* 背景轨道（深灰） */
    tft_draw_hline(x, y + 1, w, COLOR_DARK_GRAY);

    /* 已播放部分（亮色） */
    int filled = w * permille / 1000;
    if (filled > 0) {
        tft_draw_hline(x, y + 1, filled, COLOR_ACCENT);
    }

    /* 圆点指示器（Hermes 风格：3×3 圆） */
    int dot_x = x + filled;
    int dot_y = y;
    tft_fill_rect(dot_x - 1, dot_y, 3, 3, COLOR_WHITE);
}

/* ── 差异更新的缓存状态 ── */
static struct {
    char title[32];
    char subtitle[32];
    int  state;
    int  volume;
    int  position_sec;
    int  duration_sec;
    int  permille;
    char lyric_lines[UI_LYRIC_LINES][LYRIC_MAX_LEN];
    int  lyric_current;
    bool initialized;
} s_prev;

static const char *state_str[] = { "[ ]", ">>", "||" };
static const uint16_t state_clr[] = { COLOR_GRAY, COLOR_GREEN, COLOR_YELLOW };

/* 格式化时间 mm:ss */
static void fmt_time(char *buf, int buflen, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(buf, buflen, "%d:%02d", sec / 60, sec % 60);
}

void tft_ui_init_screen(void)
{
    tft_fill_screen(COLOR_BG);

    /* 分隔线 */
    tft_draw_hline(UI_MARGIN, UI_DIVIDER_Y, TFT_W - UI_MARGIN * 2, COLOR_DARK_GRAY);
    tft_draw_hline(UI_MARGIN, UI_BAR_Y, TFT_W - UI_MARGIN * 2, COLOR_DARK_GRAY);

    /* 初始文字 */
    tft_draw_string(UI_MARGIN, UI_META_Y, "DLNA Player", COLOR_META_TITLE, COLOR_BG, 1);
    tft_draw_string(UI_MARGIN, UI_META_SUB_Y, "Waiting...", COLOR_META_SUB, COLOR_BG, 1);

    memset(&s_prev, 0, sizeof(s_prev));
    s_prev.initialized = true;
}

void tft_ui_update(const tft_ui_state_t *ui)
{
    if (!ui || !s_prev.initialized) return;
    int max_w = TFT_W - UI_MARGIN * 2;

    /* ── 标题 ── */
    const char *title = ui->title ? ui->title : "";
    if (strncmp(s_prev.title, title, sizeof(s_prev.title) - 1) != 0) {
        strncpy(s_prev.title, title, sizeof(s_prev.title) - 1);
        s_prev.title[sizeof(s_prev.title) - 1] = '\0';
        tft_draw_string_truncate(UI_MARGIN, UI_META_Y, title, max_w,
                                  COLOR_META_TITLE, COLOR_BG, 1);
    }

    /* ── 副标题（歌手-专辑）── */
    const char *sub = ui->subtitle ? ui->subtitle : "";
    if (strncmp(s_prev.subtitle, sub, sizeof(s_prev.subtitle) - 1) != 0) {
        strncpy(s_prev.subtitle, sub, sizeof(s_prev.subtitle) - 1);
        s_prev.subtitle[sizeof(s_prev.subtitle) - 1] = '\0';
        tft_draw_string_truncate(UI_MARGIN, UI_META_SUB_Y, sub, max_w,
                                  COLOR_META_SUB, COLOR_BG, 1);
    }

    /* ── 歌词区（Hermes 风格：当前行白+粗体，其余灰色）── */
    if (ui->lyrics) {
        const tft_lyric_t *lyr = ui->lyrics;
        int base_y = UI_LYRIC_START_Y;

        for (int i = 0; i < UI_LYRIC_LINES; i++) {
            /* 计算源歌词行索引（当前行居中显示） */
            int src = lyr->current_line - UI_LYRIC_ACT_IDX + i;
            const char *text = "";
            char buf[LYRIC_MAX_LEN];
            if (src >= 0 && src < LYRIC_LINES && lyr->lines[src][0]) {
                text = lyr->lines[src];
            }

            /* 检查是否变化 */
            bool changed = (lyr->current_line != s_prev.lyric_current) ||
                           (strcmp(s_prev.lyric_lines[i], text) != 0);
            if (!changed) continue;

            strncpy(s_prev.lyric_lines[i], text, LYRIC_MAX_LEN - 1);
            s_prev.lyric_current = lyr->current_line;

            int ly = base_y + i * UI_LYRIC_LINE_H;
            int max_lw = TFT_W - UI_MARGIN * 2;

            if (i == UI_LYRIC_ACT_IDX && text[0]) {
                /* 当前行：白色粗体（Hermes 风格高亮） */
                tft_draw_string_truncate(UI_MARGIN, ly, text, max_lw,
                                          COLOR_LYRIC_ACT, COLOR_BG, 1);
                /* 粗体效果：再画一遍偏移1px */
                tft_draw_string_truncate(UI_MARGIN + 1, ly, text, max_lw - 1,
                                          COLOR_LYRIC_ACT, COLOR_BG, 1);
            } else {
                /* 非当前行：灰色 */
                tft_draw_string_truncate(UI_MARGIN, ly, text, max_lw,
                                          COLOR_LYRIC_INACT, COLOR_BG, 1);
            }
        }
    }

    /* ── 播放状态 + 音量 ── */
    if (ui->state != s_prev.state || ui->volume != s_prev.volume) {
        s_prev.state = ui->state;
        s_prev.volume = ui->volume;

        int st = (ui->state >= 0 && ui->state <= 2) ? ui->state : 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  VOL:%3d%%", state_str[st], ui->volume);
        tft_draw_string_truncate(UI_MARGIN, UI_STATUS_Y, buf, max_w,
                                  state_clr[st], COLOR_BG, 1);
    }

    /* ── 进度条（Hermes 风格）── */
    int permille = (ui->duration_sec > 0) ?
                   (ui->position_sec * 1000 / ui->duration_sec) : 0;
    if (permille != s_prev.permille || ui->duration_sec != s_prev.duration_sec) {
        s_prev.permille = permille;
        s_prev.duration_sec = ui->duration_sec;
        s_prev.position_sec = ui->position_sec;

        /* 清除旧进度条 */
        tft_fill_rect(UI_MARGIN, UI_PROGRESS_Y, max_w, 4, COLOR_BG);
        tft_draw_progress_bar(UI_MARGIN, UI_PROGRESS_Y, max_w, permille);

        /* 时间标签 */
        char t1[12], t2[12];
        fmt_time(t1, sizeof(t1), ui->position_sec);
        fmt_time(t2, sizeof(t2), ui->duration_sec);
        char time_str[28];
        snprintf(time_str, sizeof(time_str), "%s / %s", t1, t2);
        tft_draw_string_truncate(UI_MARGIN, UI_TIME_Y, time_str, max_w,
                                  COLOR_LIGHT_GRAY, COLOR_BG, 1);
    }
}