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

static const char *TAG = "LVGL_INDEV";

static lv_indev_drv_t  s_indev_drv;
static lv_indev_t     *s_indev;

/* ── 全局旋钮位置计数器（由回调更新，由 encoder_read 消费）── */
static volatile int s_enc_position = 0;
static volatile int s_enc_btn      = 0;

/* 回调：旋钮旋转 */
static void _on_rotate(void *arg, int direction)
{
    (void)arg;
    s_enc_position += direction;
}

/* 回调：旋钮按键 */
static void _on_btn_click(void *arg)
{
    (void)arg;
    s_enc_btn = 1;
}

/* ── 旋钮读取回调（LVGL 周期性调用）── */
static void encoder_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    /* 读取并清除位置差 */
    int pos = s_enc_position;
    data->enc_diff = pos;
    s_enc_position -= pos;  /* 归零已消费的差值 */

    /* 按键状态 */
    if (s_enc_btn) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = LV_KEY_ENTER;
        s_enc_btn = 0;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
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

    ESP_LOGI(TAG, "Encoder input registered");
}

/* ── 获取旋钮回调函数（供 rotary_encoder_init 调用）── */
void lvgl_port_get_encoder_callbacks(void (**on_rotate)(void*, int),
                                     void (**on_btn_click)(void*))
{
    if (on_rotate)    *on_rotate = _on_rotate;
    if (on_btn_click) *on_btn_click = _on_btn_click;
}