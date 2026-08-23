/*
 * LVGL 显示驱动 — ST7735S SPI flush 回调
 *
 * 使用双缓冲 (1/4 屏幕) + PSRAM，通过 tft_write_rect 写入 SPI
 */

#include "lvgl_port.h"
#include "lvgl.h"
#include "tft_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "LVGL_DISP";

/* ── LVGL 显示缓冲（静态 DMA 内存，避免运行时分配失败）── */
#define DISP_BUF_ROWS  20       /* 1/8 屏幕行数，节省内部 RAM */
static uint8_t s_buf1_data[TFT_W * DISP_BUF_ROWS * sizeof(lv_color_t)] __attribute__((aligned(4)));
static uint8_t s_buf2_data[TFT_W * DISP_BUF_ROWS * sizeof(lv_color_t)] __attribute__((aligned(4)));
static lv_color_t *s_buf1;
static lv_color_t *s_buf2;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t  s_disp_drv;
static lv_disp_t     *s_disp;

/* ── LVGL 互斥锁 ── */
static SemaphoreHandle_t s_lvgl_mux;

/* ── SPI flush 回调 ── */
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    tft_write_rect(area->x1, area->y1, w, h, (const uint16_t *)color_p);
    lv_disp_flush_ready(drv);
}

/* ── LVGL 定时器处理任务 ── */
static void lvgl_tick_task(void *arg)
{
    int tick = 0;
    while (1) {
        lv_tick_inc(5); /* 告诉 LVGL 过去了 5ms，驱动内部时钟 */
        lvgl_port_lock();
        lv_timer_handler();
        lvgl_port_unlock();
        if ((++tick % 200) == 0) ESP_LOGI("LVGL_TICK", "alive (heap=%d)", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ── 初始化 ── */
esp_err_t lvgl_port_init(int task_priority)
{
    /* 互斥锁 */
    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) return ESP_FAIL;

    /* 分配显示缓冲（静态 BSS，DMA 安全，无需堆分配） */
    s_buf1 = (lv_color_t *)s_buf1_data;
    s_buf2 = (lv_color_t *)s_buf2_data;

    /* LVGL 初始化 */
    lv_init();

    /* 注册显示驱动 */
    lv_disp_draw_buf_init(&s_disp_buf, s_buf1, s_buf2, TFT_W * DISP_BUF_ROWS);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = TFT_W;
    s_disp_drv.ver_res = TFT_H;
    s_disp_drv.flush_cb = disp_flush;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    /* 旋钮输入（必须在 UI 创建前，让 group 已存在） */
    lvgl_port_indev_init();

    /* 创建 UI */
    lvgl_port_ui_create();

    /* 创建 LVGL 处理任务 */
    BaseType_t ret = xTaskCreatePinnedToCore(lvgl_tick_task, "lvgl_tick", 4096, NULL,
                            task_priority, NULL, 0);
    if (ret != pdPASS) ESP_LOGE(TAG, "lvgl_tick_task create FAILED (heap=%d)", esp_get_free_heap_size());
    else ESP_LOGI(TAG, "lvgl_tick_task created OK");

    ESP_LOGI(TAG, "LVGL ready (%dx%d, %dKB static DMA buffers)", TFT_W, TFT_H,
             (int)(2 * sizeof(s_buf1_data) / 1024));
    return ESP_OK;
}

/* ── 互斥锁 ── */
void lvgl_port_lock(void)
{
    if (s_lvgl_mux) xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
}

void lvgl_port_unlock(void)
{
    if (s_lvgl_mux) xSemaphoreGive(s_lvgl_mux);
}