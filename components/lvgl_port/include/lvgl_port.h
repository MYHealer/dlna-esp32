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
 * @brief 注册旋钮编码器为 LVGL 输入设备（创建焦点组）
 */
void lvgl_port_indev_init(void);

/**
 * @brief 获取旋钮回调（供 rotary_encoder_init 接线到 LVGL）
 */
void lvgl_port_get_encoder_callbacks(void (**on_rotate)(void*, int),
                                     void (**on_btn_click)(void*));

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

/**
 * @brief 小米音箱模式：显示/隐藏接管画面
 * @param active true=显示"音频已由手机接管", false=恢复正常UI
 */
void lvgl_port_ui_set_speaker_mode(bool active);

/* ══════════════════════════════════════════════
 *  旋钮控制按钮回调注册
 * ══════════════════════════════════════════════ */
typedef void (*lvgl_btn_cb_t)(void);
void lvgl_port_ui_register_btn_prev_cb(lvgl_btn_cb_t cb);
void lvgl_port_ui_register_btn_play_cb(lvgl_btn_cb_t cb);
void lvgl_port_ui_register_btn_next_cb(lvgl_btn_cb_t cb);

/**
 * @brief 显示/隐藏焦点框（旋转时显示，10s 无操作后隐藏）
 */
void lvgl_port_ui_set_focus_visible(bool visible);

/**
 * @brief 焦点归位到播放按钮
 */
void lvgl_port_ui_focus_play(void);

#ifdef __cplusplus
}
#endif