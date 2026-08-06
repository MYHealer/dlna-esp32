/* idf6_compat.h — IDF 6.2 兼容层，通过 CMake -include 全局注入
 * 解决 ESP-ADF v2.x 在 IDF 6.x 上缺少头文件的问题
 * 仅预处理器宏，零运行时开销
 * IDF 5.x (S3) 构建时大部分定义被版本守卫跳过 */
#pragma once

/* 汇编文件不能包含 C 头文件，跳过 */
#ifndef __ASSEMBLER__

/* esp_codec_dev 使用 ESP_IDF_VERSION_VAL 但没 include esp_idf_version.h */
#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)

/* audio_hal 的 tas5805m/es8311 等使用 gpio_config_t 但没 include driver/gpio.h */
#if __has_include("driver/gpio.h")
#include "driver/gpio.h"
#endif

/* HSPI_HOST 在 IDF 6.x 已移除 */
#ifndef HSPI_HOST
#define HSPI_HOST SPI3_HOST
#endif

/* IDF 6.x 重命名了 UART VFS 函数 */
#define esp_vfs_dev_uart_port_set_rx_line_endings uart_vfs_dev_port_set_rx_line_endings
#define esp_vfs_dev_uart_port_set_tx_line_endings uart_vfs_dev_port_set_tx_line_endings
#define esp_vfs_dev_uart_use_driver uart_vfs_dev_use_driver

/* IDF 6.x 移除了 i2s_port_t 和 SOC_I2S_NUM（I2S 驱动改为句柄模型） */
#if __has_include("driver/i2s_std.h")
#include "driver/i2s_types.h"
#ifndef SOC_I2S_NUM
#define SOC_I2S_NUM 2
#endif
typedef int i2s_port_t;
#endif

/* IDF 6.x WiFi 接口枚举重命名 */
#ifndef ESP_IF_WIFI_STA
#define ESP_IF_WIFI_STA  WIFI_IF_STA
#endif
#ifndef ESP_IF_WIFI_AP
#define ESP_IF_WIFI_AP   WIFI_IF_AP
#endif

#endif /* ESP_IDF_VERSION >= 6.0.0 */

#endif /* __ASSEMBLER__ */
