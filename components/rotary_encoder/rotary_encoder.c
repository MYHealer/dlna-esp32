/*
 * Rotary encoder driver for ESP32-S3
 *
 * Uses PCNT (Pulse Counter) hardware peripheral for reliable
 * quadrature decoding with built-in filter and edge counting.
 */

#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/pcnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "rotary_encoder.h"

static const char *TAG = "ROTARY_ENC";

#define PCNT_UNIT  PCNT_UNIT_0
#define PCNT_CH    PCNT_CHANNEL_0

#define BTN_DEBOUNCE_MS     20

/* Encoder and button state */
static struct {
    const rotary_encoder_config_t *cfg;
    QueueHandle_t evt_queue;
} enc;

typedef enum {
    ENC_EVT_ROTATION = 0,
    ENC_EVT_BTN_CLICK,
} enc_evt_type_t;

typedef struct {
    enc_evt_type_t type;
    int            direction;
} enc_evt_t;

/* Previous count for detecting direction changes */
static int16_t s_last_count = 0;

/* ---------------------------------------------------------------
 * PCNT watch task – accumulates delta, fires on rotation stop
 * --------------------------------------------------------------- */
#define ROTATION_SETTLE_MS  300

static void pcnt_task(void *arg)
{
    (void)arg;
    int16_t count = 0;
    int accumulated = 0;
    bool rotating = false;
    int64_t last_change_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        pcnt_get_counter_value(PCNT_UNIT, &count);

        int16_t delta = count - s_last_count;
        if (delta != 0) {
            s_last_count = count;
            accumulated += delta;
            last_change_time = esp_timer_get_time() / 1000;
            rotating = true;
        } else if (rotating && (esp_timer_get_time() / 1000 - last_change_time) >= ROTATION_SETTLE_MS) {
            int dir = (accumulated > 0) ? 1 : -1;
            enc_evt_t evt = { .type = ENC_EVT_ROTATION, .direction = dir };
            xQueueSend(enc.evt_queue, &evt, 0);
            accumulated = 0;
            rotating = false;
        }
    }
}

/* ---------------------------------------------------------------
 * ISR handler for button (falling edge = press)
 * --------------------------------------------------------------- */
static void IRAM_ATTR btn_isr(void *arg)
{
    enc_evt_t evt = { .type = ENC_EVT_BTN_CLICK, .direction = 0 };
    BaseType_t task_wake = pdFALSE;
    xQueueSendFromISR(enc.evt_queue, &evt, &task_wake);
    if (task_wake) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------------------------------------------------------
 * Processing task – runs callbacks in thread context
 * --------------------------------------------------------------- */
static void rotary_task(void *arg)
{
    enc_evt_t evt;
    int pin_btn = enc.cfg->sw_gpio;
    (void)arg;

    while (1) {
        if (xQueueReceive(enc.evt_queue, &evt, portMAX_DELAY)) {
            switch (evt.type) {
            case ENC_EVT_ROTATION:
                if (enc.cfg->on_rotate) {
                    enc.cfg->on_rotate(enc.cfg->arg, evt.direction);
                }
                break;

            case ENC_EVT_BTN_CLICK:
                vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
                if (gpio_get_level(pin_btn) == 0) {
                    /* Wait for button release */
                    while (gpio_get_level(pin_btn) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    if (enc.cfg->on_btn_click) {
                        enc.cfg->on_btn_click(enc.cfg->arg);
                    }
                }
                break;
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

    enc.cfg = config;
    enc.evt_queue = xQueueCreate(32, sizeof(enc_evt_t));
    if (enc.evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* ── Configure PCNT for quadrature decoding ── */
    pcnt_config_t pcnt_cfg = {
        .pulse_gpio_num = config->clk_gpio,
        .ctrl_gpio_num  = config->dt_gpio,
        .channel        = PCNT_CHANNEL_0,
        .unit           = PCNT_UNIT,
        .pos_mode       = PCNT_COUNT_INC,   /* rising edge of CLK → count up */
        .neg_mode       = PCNT_COUNT_DEC,   /* falling edge of CLK → count down */
        .lctrl_mode     = PCNT_MODE_REVERSE, /* DT high reverses count direction */
        .hctrl_mode     = PCNT_MODE_KEEP,    /* DT low keeps direction */
        .counter_h_lim  = 32767,
        .counter_l_lim  = -32768,
    };
    esp_err_t ret = pcnt_unit_config(&pcnt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_unit_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── Hardware filter: 1000 APB clock cycles ≈ 12.5µs debounce ── */
    pcnt_set_filter_value(PCNT_UNIT, 1000);
    pcnt_filter_enable(PCNT_UNIT);

    /* ── Clear and start counter ── */
    pcnt_counter_pause(PCNT_UNIT);
    pcnt_counter_clear(PCNT_UNIT);
    pcnt_counter_resume(PCNT_UNIT);

    /* ── Button GPIO ── */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << config->sw_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(config->sw_gpio, btn_isr, NULL);

    /* ── Start tasks ── */
    xTaskCreate(pcnt_task, "pcnt_read", 4096, NULL, 8, NULL);
    xTaskCreate(rotary_task, "rotary_enc", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Init OK (CLK=%d, DT=%d, SW=%d, PCNT)", config->clk_gpio, config->dt_gpio, config->sw_gpio);
    return ESP_OK;
}
