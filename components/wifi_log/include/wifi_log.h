#pragma once

#include "esp_netif_types.h"

/**
 * @brief 启动 WiFi UDP 日志流
 *
 * 设备启动后所有 ESP_LOG 自动通过 UDP 发到 PC。
 * PC 端接收:
 *   Windows:  python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('',4444));[print(s.recvfrom(4096)[0].decode(),end='') for _ in iter(int,1)]"
 *   Linux:    nc -u -l 4444
 */
void wifi_log_init(void);
