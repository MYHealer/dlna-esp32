/*
 * Rotary encoder driver for ESP32-S3 — 查表法（lookup table / state machine）
 *
 * 相比 PCNT 方案：
 *   - PCNT + 300ms 静置上报：一次旋转只报一次方向，且要等停转，UI 导航迟钝
 *   - 查表法：GPIO 中断在每次电平跳变即时解码方向，每格（detent）= 4 次跳变，
 *     处理任务按 4 归一键上报，响应即时，适合 LVGL 焦点框选
 */

#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "rotary_encoder.h"

static const char *TAG = "ROTARY_ENC";

#define BTN_DEBOUNCE_MS     20
#define ENC_POLL_MS         10
#define DETENT_STATES       4   /* 每格 = 4 次状态跳变 */

/* 方向查找表：索引 = [prev_AB(2bit) << 2 | now_AB(2bit)]，0=非法跳变（抖动） */
/* 放 DRAM：ISR 中访问，避免 flash cache 缺页 */
DRAM_ATTR static const int8_t direction_lookup[16] = {
    0,  1, -1,  0,
   -1,  0,  0,  1,
    1,  0,  0, -1,
    0, -1,  1,  0
};

typedef enum {
    ENC_EVT_ROTATION = 0,
    ENC_EVT_BTN_CLICK,
} enc_evt_type_t;

typedef struct {
    enc_evt_type_t type;
    int            direction;
} enc_evt_t;

static struct {
    rotary_encoder_config_t cfg;   /* 复制配置，避免调用方栈上指针悬垂 */
    QueueHandle_t evt_queue;
    int            clk_gpio;     /* 缓存到 RAM，ISR 不读 flash 配置 */
    int            dt_gpio;
    volatile int32_t  enc_pos;   /* 原始正交计数（每格=4） */
    volatile uint8_t  old_ab;    /* A/B 状态历史 */
    int32_t last_pos;            /* 处理任务上次读数 */
    int32_t remainder;           /* 除以4的余数累积（处理半格） */
} enc;

/* ---------------------------------------------------------------
 * ISR — 查表解码旋转方向
 * --------------------------------------------------------------- */
static void IRAM_ATTR enc_rot_isr(void *arg)
{
    (void)arg;
    uint8_t a = gpio_get_level(enc.clk_gpio);
    uint8_t b = gpio_get_level(enc.dt_gpio);
    uint8_t ab = (uint8_t)((a << 1) | b);
    enc.old_ab = (uint8_t)((enc.old_ab << 2) | ab) & 0x0F;
    int8_t dir = direction_lookup[enc.old_ab];
    if (dir != 0) {
        enc.enc_pos += dir;
    }
}

/* ---------------------------------------------------------------
 * ISR — 按键（下降沿）
 * --------------------------------------------------------------- */
static void IRAM_ATTR btn_isr(void *arg)
{
    (void)arg;
    enc_evt_t evt = { .type = ENC_EVT_BTN_CLICK, .direction = 0 };
    BaseType_t task_wake = pdFALSE;
    xQueueSendFromISR(enc.evt_queue, &evt, &task_wake);
    if (task_wake) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------------------------------------------------------
 * 处理任务 — 轮询计数按格上报 + 消费按键事件
 * --------------------------------------------------------------- */
static void rotary_task(void *arg)
{
    (void)arg;
    enc_evt_t evt;

    while (1) {
        /* 旋转：轮询查表计数，每 DETENT_STATES 次跳变上报一次方向 */
        int32_t pos = enc.enc_pos;
        int32_t delta = pos - enc.last_pos;
        if (delta != 0) {
            enc.last_pos = pos;
            enc.remainder += delta;
            int32_t detents = enc.remainder / DETENT_STATES;
            enc.remainder %= DETENT_STATES;
            enc_evt_t revt = { .type = ENC_EVT_ROTATION, .direction = 0 };
            while (detents > 0) {
                revt.direction = 1;
                xQueueSend(enc.evt_queue, &revt, 0);
                detents--;
            }
            while (detents < 0) {
                revt.direction = -1;
                xQueueSend(enc.evt_queue, &revt, 0);
                detents++;
            }
        }

        /* 按键事件（阻塞最多 ENC_POLL_MS 等一个事件） */
        if (xQueueReceive(enc.evt_queue, &evt, pdMS_TO_TICKS(ENC_POLL_MS))) {
            if (evt.type == ENC_EVT_ROTATION) {
                if (enc.cfg.on_rotate) {
                    enc.cfg.on_rotate(enc.cfg.arg, evt.direction);
                }
            } else { /* BTN_CLICK */
                int pin_btn = enc.cfg.sw_gpio;
                vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
                if (gpio_get_level(pin_btn) == 0) {
                    /* 等待释放 */
                    while (gpio_get_level(pin_btn) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    if (enc.cfg.on_btn_click) {
                        enc.cfg.on_btn_click(enc.cfg.arg);
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */
esp_err_t rotary_encoder_init(const rotary_encoder_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&enc.cfg, config, sizeof(rotary_encoder_config_t));
    enc.clk_gpio = config->clk_gpio;
    enc.dt_gpio  = config->dt_gpio;
    enc.evt_queue = xQueueCreate(32, sizeof(enc_evt_t));
    if (enc.evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* 初始化 A/B 状态历史为当前电平，避免首次中断误判 */
    uint8_t a = gpio_get_level(config->clk_gpio);
    uint8_t b = gpio_get_level(config->dt_gpio);
    enc.old_ab = (uint8_t)((a << 1) | b);
    enc.enc_pos = 0;
    enc.last_pos = 0;
    enc.remainder = 0;

    /* ── GPIO 输入（任意沿触发中断，查表法需要 A/B 每次变化都解码）── */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << config->clk_gpio) | (1ULL << config->dt_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_cfg);

    /* ── 按键 GPIO ── */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << config->sw_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);

    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── 注册中断：A/B 任意电平变化 → 查表解码 ── */
    gpio_isr_handler_add(config->clk_gpio, enc_rot_isr, NULL);
    gpio_isr_handler_add(config->dt_gpio, enc_rot_isr, NULL);
    gpio_isr_handler_add(config->sw_gpio, btn_isr, NULL);

    /* ── 启动处理任务 ── */
    xTaskCreate(rotary_task, "rotary_enc", 4096, NULL, 8, NULL);

    ESP_LOGI(TAG, "Init OK (CLK=%d, DT=%d, SW=%d, lookup-table)", config->clk_gpio, config->dt_gpio, config->sw_gpio);
    return ESP_OK;
}