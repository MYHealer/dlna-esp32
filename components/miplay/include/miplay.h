#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 MiPlay mDNS 广播 + TCP 8899 监听
 *
 * 注册 _lyra-mdns._udp 和 _mi-connect._udp mDNS 服务，
 * 让小米妙播手机端发现 ESP32 设备。
 */
esp_err_t miplay_init(void);

/**
 * @brief 停止 MiPlay 服务
 */
void miplay_stop(void);

/**
 * @brief MiPlay 连接状态回调
 * connected=true: 手机已连上 MiPlay，DLNA 应暂停
 * connected=false: MiPlay 断开，DLNA 可恢复
 */
typedef void (*miplay_connected_cb_t)(bool connected);
void miplay_set_connected_cb(miplay_connected_cb_t cb);

/**
 * @brief 获取当前 MiPlay 音量百分比 (0-100)
 */
uint32_t miplay_get_volume(void);

/**
 * @brief MiPlay 音量变化回调（手机端设置音量时触发）
 */
typedef void (*miplay_vol_changed_cb_t)(uint32_t vol_percent);
void miplay_set_vol_changed_cb(miplay_vol_changed_cb_t cb);

/**
 * @brief 向手机发送反向控制通知
 * @param action "pause", "play", "next", "prev", "seek"
 * @param value  seek 时为 positionMs，其他为 0
 */
void miplay_send_receiver_control(const char *action, int64_t value);

/**
 * @brief 设置 MiPlay TS 数据输出的 ring buffer
 */
void miplay_set_ts_ringbuf(RingbufHandle_t rb);

/**
 * @brief MiPlay 媒体流开始/停止回调
 * media_receive_task 启动时触发 start=true，结束时触发 start=false
 */
typedef void (*miplay_media_cb_t)(bool start);
void miplay_set_media_cb(miplay_media_cb_t cb);

#ifdef __cplusplus
}
#endif
