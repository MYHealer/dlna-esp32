/*
 * Board pin definitions for ESP32-S3 + PCM5102A
 *
 * I2S 引脚 & MCLK:
 *   LRCK=GPIO16, BCK=GPIO15, DOUT=GPIO7, MCLK=GPIO17
 *   MCLK 频率 = 256×fs（44.1kHz→11.29MHz, 48kHz→12.29MHz）
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#include "driver/gpio.h"

/* PCM5102A I2S 引脚 */
#define GPIO_I2S_LRCK       (GPIO_NUM_16)
#define GPIO_I2S_SCLK       (GPIO_NUM_15)
#define GPIO_I2S_DOUT       (GPIO_NUM_7)
#define GPIO_I2S_MCLK       (GPIO_NUM_17)

#endif
