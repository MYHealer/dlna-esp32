/*
 * LVGL 端口桥接 — ESP32-S3 + ST7735S + 旋钮
 *
 * 提供：
 *   lvgl_port_init()         — 初始化 LVGL + 显示 + 输入
 *   lvgl_port_lock()/unlock() — 线程安全访问
 *   lvgl_port_ui_*()          — UI 控件更新
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL + 显示驱动 + 旋钮输入
 * @param task_priority  LVGL 定时器处理任务优先级
 * @return ESP_OK 或错误码
 */
esp_err_t lvgl_port_init(int task_priority);

/**
 * @brief LVGL 互斥锁（display_task 中调用 lvgl 前必须 lock）
 */
void lvgl_port_lock(void);
void lvgl_port_unlock(void);

/* ══════════════════════════════════════════════
 *  UI 创建 + 更新接口
 * ══════════════════════════════════════════════ */

void lvgl_port_ui_create(void);
void lvgl_port_ui_set_title(const char *title);
void lvgl_port_ui_set_artist(const char *artist);
void lvgl_port_ui_set_progress(int position_sec, int duration_sec);
void lvgl_port_ui_set_state(int state);  /* 0=停止 1=播放 2=暂停 */
void lvgl_port_ui_set_volume(int vol);
void lvgl_port_ui_lyrics_update(int current_idx, const char *prev, const char *curr, const char *next);
void lvgl_port_ui_lyrics_karaoke(int byte_idx);
void lvgl_port_ui_lyrics_scroll_to_end(int line_duration_ms);
void lvgl_port_ui_lyrics_tick_scroll(void);
void lvgl_port_ui_set_cover(const uint16_t *pixels, int w, int h);
void lvgl_port_ui_lyrics_create(void);
void lvgl_port_ui_toggle_lyrics(void);
bool lvgl_port_ui_lyrics_is_visible(void);
void lvgl_port_ui_lyrics_clear(void);

#ifdef __cplusplus
}
#endif