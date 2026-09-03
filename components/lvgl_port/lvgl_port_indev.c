/*
 * LVGL 旋钮输入驱动 — 使用 rotary_encoder 组件回调
 *
 * 映射：
 *   旋钮旋转 → LV_INDEV_TYPE_ENCODER (enc_diff)
 *   旋钮按键 → LV_KEY_ENTER
 */

#include "lvgl_port.h"
#include "lvgl.h"
#include "rotary_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LVGL_INDEV";

static lv_indev_drv_t  s_indev_drv;
static lv_indev_t     *s_indev;

/* ── 全局旋钮位置计数器（由回调更新，由 encoder_read 消费）── */
static volatile int s_enc_position = 0;
static volatile int s_enc_btn      = 0;

/* ── 焦点框显示控制：旋转/按键显示，10s 无操作隐藏归位 ── */
#define FOCUS_TIMEOUT_US  10000000LL  /* 10s */
static volatile int64_t s_last_activity_us = 0;
static volatile bool   s_pending_focus_show = false;
static bool            s_focus_visible = false;

/* 回调：旋钮旋转（方向取反：物理接线方向与 LVGL 期望相反） */
static void _on_rotate(void *arg, int direction)
{
    (void)arg;
    s_enc_position -= direction;
    s_last_activity_us = esp_timer_get_time();
    s_pending_focus_show = true;
    ESP_LOGI(TAG, "rotate dir=%d", direction);
}

/* 回调：旋钮按键 */
static void _on_btn_click(void *arg)
{
    (void)arg;
    s_enc_btn = 1;
    s_last_activity_us = esp_timer_get_time();
    s_pending_focus_show = true;
    ESP_LOGI(TAG, "BTN pressed (ISR)");
}

/* ── 旋钮读取回调（LVGL 周期性调用）── */
static void encoder_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    /* 读取并清除位置差 */
    int pos = s_enc_position;
    data->enc_diff = pos;
    s_enc_position -= pos;  /* 归零已消费的差值 */
    if (pos) ESP_LOGI(TAG, "enc_diff=%d", pos);

    /* 按键：直接触发聚焦按钮 CLICKED（一次确认，不走 ENTER/editing 模式） */
    if (s_enc_btn) {
        s_enc_btn = 0;
        lv_group_t *g = lv_group_get_default();
        if (g) {
            lv_obj_t *focused = lv_group_get_focused(g);
            if (focused) {
                ESP_LOGI(TAG, "BTN consumed -> CLICKED on %p", focused);
                lv_event_send(focused, LV_EVENT_CLICKED, NULL);
            } else {
                ESP_LOGW(TAG, "BTN consumed but no focused obj");
            }
        } else {
            ESP_LOGW(TAG, "BTN consumed but no group");
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;

    /* 旋转/按键 → 显示焦点框 */
    if (s_pending_focus_show) {
        s_pending_focus_show = false;
        if (!s_focus_visible) {
            s_focus_visible = true;
            lvgl_port_ui_set_focus_visible(true);
        }
    }

    /* 10s 无操作 → 隐藏焦点框，归位到播放按钮 */
    if (s_focus_visible &&
        (esp_timer_get_time() - s_last_activity_us > FOCUS_TIMEOUT_US)) {
        s_focus_visible = false;
        lvgl_port_ui_set_focus_visible(false);
        lvgl_port_ui_focus_play();
        lv_group_t *g = lv_group_get_default();
        if (g) lv_group_set_editing(g, false);
    }
}

/* ── 初始化 ── */
void lvgl_port_indev_init(void)
{
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_ENCODER;
    s_indev_drv.read_cb = encoder_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);

    /* 创建旋钮组，让 LVGL 可以焦点切换 */
    lv_group_t *g = lv_group_create();
    lv_indev_set_group(s_indev, g);
    lv_group_set_default(g);
    lv_group_set_wrap(g, false);  /* 不循环：到边界后旋转无响应 */

    ESP_LOGI(TAG, "Encoder input registered");
}

/* ── 获取旋钮回调函数（供 rotary_encoder_init 调用）── */
void lvgl_port_get_encoder_callbacks(void (**on_rotate)(void*, int),
                                     void (**on_btn_click)(void*))
{
    if (on_rotate)    *on_rotate = _on_rotate;
    if (on_btn_click) *on_btn_click = _on_btn_click;
}