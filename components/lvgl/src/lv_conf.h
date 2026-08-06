/**
 * @file lv_conf.h
 * LVGL 8.4.0 配置 — ST7735S 160x128 RGB565, ESP32-S3 + PSRAM
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── 色彩 ── */
#define LV_COLOR_DEPTH         16
#define LV_COLOR_16_SWAP       1
#define LV_COLOR_SCREEN_TRANSP 0

/* ── 内存 — 64KB LVGL 堆，从 PSRAM 分配 ── */
#define LV_MEM_CUSTOM          0
#define LV_MEM_SIZE            (64 * 1024)

/* ── 显示 ── */
#define LV_DPI_DEFAULT         130

/* ── 启用组件 ── */
#define LV_USE_LABEL           1
#define LV_USE_BAR             1
#define LV_USE_IMG             1
#define LV_USE_ANIMATION       1
#define LV_USE_THEME_DEFAULT   1
#define LV_USE_THEME_BASIC     1
#define LV_USE_SYSMON          0

/* ── 字体 ── */
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_14

/* ── 日志 ── */
#define LV_USE_LOG             0

/* ── 杂项 ── */
#define LV_USE_OBJ_ID          0
#define LV_USE_OBJ_ASSERT      0

#endif /* LV_CONF_H */