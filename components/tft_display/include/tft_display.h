/*
 * ST7735S 1.8" TFT 显示驱动 + Hermes 风格 UI
 * 128×160 (横屏 160×128) RGB565, SPI DMA
 *
 * UI 布局参考 Hermes (DuoDuoJuZi)：
 *   - 标题+歌手信息区
 *   - 歌词区（当前行高亮白+粗体，其余灰色）
 *   - 进度条（细线+圆点指示器）
 *   - 播放状态+音量
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 引脚配置 ── */
#define TFT_SPI_HOST    SPI2_HOST
#define TFT_PIN_SCLK    12
#define TFT_PIN_MOSI    11
#define TFT_PIN_CS      10
#define TFT_PIN_DC       9
#define TFT_PIN_RST      8
#define TFT_PIN_BL       3

/* ── 屏幕参数（竖屏：宽128 高160）── */
#define TFT_W           128
#define TFT_H           160
#define TFT_SPI_FREQ_HZ (40 * 1000 * 1000)

/* ── RGB565 颜色 ── */
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_BLUE        0x001F
#define COLOR_YELLOW      0xFFE0
#define COLOR_CYAN        0x07FF
#define COLOR_MAGENTA     0xF81F
#define COLOR_ORANGE      0xFD20
#define COLOR_GRAY        0x8410
#define COLOR_DARK_GRAY   0x4208
#define COLOR_LIGHT_GRAY  0xC618

/* Hermes 主题色（深色背景 + 亮色点缀） */
#define COLOR_BG          COLOR_BLACK
#define COLOR_THEME       0x10A2     /* 深绿灰，模拟 Hermes 暗主题色 */
#define COLOR_LYRIC_ACT   COLOR_WHITE
#define COLOR_LYRIC_INACT COLOR_GRAY
#define COLOR_META_TITLE  COLOR_CYAN
#define COLOR_META_SUB    COLOR_LIGHT_GRAY
#define COLOR_ACCENT      0x07FF     /* 青色高亮 */

/* ── UI 布局常量（横屏坐标）── */
#define UI_MARGIN        2
#define UI_META_Y        2           /* 标题行 y */
#define UI_META_SUB_Y    12          /* 副标题 y */
#define UI_DIVIDER_Y     21          /* 分隔线 y */
#define UI_LYRIC_START_Y 24          /* 歌词区起始 y */
#define UI_LYRIC_LINE_H  11          /* 歌词行高 */
#define UI_LYRIC_LINES   5           /* 显示歌词行数 */
#define UI_LYRIC_ACT_IDX 2           /* 当前歌词行索引 (0-based) */
#define UI_BAR_Y         82          /* 分隔线2 y */
#define UI_STATUS_Y      85          /* 状态行 y */
#define UI_PROGRESS_Y    97          /* 进度条 y */
#define UI_TIME_Y        102         /* 时间标签 y */
#define UI_VOL_BAR_Y     112         /* 音量条 y */

/* ── 歌词缓冲 ── */
#define LYRIC_MAX_LEN    64          /* 每行最大字符 */
#define LYRIC_LINES      11          /* 歌词缓冲总行数 */

typedef struct {
    char lines[LYRIC_LINES][LYRIC_MAX_LEN];
    int  current_line;               /* 当前播放行索引 */
} tft_lyric_t;

/* ── UI 状态（传给 tft_ui_update）── */
typedef struct {
    const char *title;               /* 歌曲名 */
    const char *subtitle;            /* 歌手 - 专辑 */
    const tft_lyric_t *lyrics;       /* 歌词（可为 NULL） */
    int  state;                      /* 0=stopped, 1=playing, 2=paused */
    int  volume;                     /* 0-100 */
    int  position_sec;               /* 当前位置（秒） */
    int  duration_sec;               /* 总时长（秒） */
} tft_ui_state_t;

/* ══════════════════════════════════════════════
 *  基础绘图 API
 * ══════════════════════════════════════════════ */

esp_err_t tft_init(void);
void      tft_set_backlight(uint8_t brightness);
void      tft_fill_screen(uint16_t color);
void      tft_fill_rect(int x, int y, int w, int h, uint16_t color);
void      tft_draw_pixel(int x, int y, uint16_t color);
void      tft_draw_hline(int x, int y, int w, uint16_t color);
void      tft_draw_vline(int x, int y, int h, uint16_t color);
void      tft_draw_rect(int x, int y, int w, int h, uint16_t color);

/* 普通字符/字符串 */
int  tft_draw_char(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size);
void tft_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size);
void tft_draw_string_truncate(int x, int y, const char *str, int max_width,
                               uint16_t color, uint16_t bg, uint8_t size);

/**
 * @brief 快速写入矩形像素数据（供 LVGL flush 回调使用）
 * @param data RGB565 像素数组，长度 w*h
 */
void tft_write_rect(int x, int y, int w, int h, const uint16_t *data);

/* ══════════════════════════════════════════════
 *  Hermes 风格 UI 组件
 * ══════════════════════════════════════════════ */

/**
 * @brief 绘制粗体字符（Hermes 风格：偏移1px重绘实现加粗）
 */
int tft_draw_char_bold(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size);

/**
 * @brief 绘制粗体字符串
 */
void tft_draw_string_bold(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size);

/**
 * @brief 绘制 Hermes 风格进度条（细线 + 圆点指示器）
 * @param permille 进度千分比 (0-1000)
 */
void tft_draw_progress_bar(int x, int y, int w, int permille);

/**
 * @brief 完整 UI 刷新（差异更新，仅重绘变化区域）
 * @param ui UI 状态
 */
void tft_ui_update(const tft_ui_state_t *ui);

/**
 * @brief UI 初始化画面（首次显示）
 */
void tft_ui_init_screen(void);

#ifdef __cplusplus
}
#endif
