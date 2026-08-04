#ifndef _ROTARY_ENCODER_H
#define _ROTARY_ENCODER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  clk_gpio;        /* CLK pin */
    int  dt_gpio;         /* DT  pin */
    int  sw_gpio;         /* SW  pin (button, pressed low) */
    int  vol_step;        /* Volume step per detent */
    void (*on_rotate)(void *arg, int direction);      /* direction: 1=CW, -1=CCW */
    void (*on_btn_click)(void *arg);                     /* single click */
    void *arg;
} rotary_encoder_config_t;

esp_err_t rotary_encoder_init(const rotary_encoder_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* _ROTARY_ENCODER_H */
