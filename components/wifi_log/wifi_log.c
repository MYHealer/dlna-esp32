#include "wifi_log.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "WIFI_LOG";

/* ── RTC 保留区：跨软件重启保存崩溃信息 ── */
typedef struct {
    uint32_t magic;           /* 0xDEADBEEF = 有效 */
    uint32_t pc;              /* 崩溃时的 PC */
    uint32_t core;            /* 哪个核 */
    uint32_t cause;           /* 异常原因 */
    uint32_t backtrace[8];    /* 回溯栈帧 */
    int      bt_depth;
} crash_info_t;

static RTC_DATA_ATTR crash_info_t s_crash_store;

/* ── 发送端 ── */
static struct sockaddr_in s_dest_addr;
static int                 s_udp_sock   = -1;
static vprintf_like_t      s_orig_vprintf = NULL;

/* PSRAM 静态缓冲：避免每个调用栈分配 512 字节 */
static EXT_RAM_BSS_ATTR char s_log_buf[512];
static SemaphoreHandle_t s_log_mutex = NULL;

static int udp_log_vprintf(const char *fmt, va_list args)
{
    if (s_orig_vprintf) {
        s_orig_vprintf(fmt, args);
    }

    if (s_udp_sock < 0) return 0;

    /* ISR 上下文不发 UDP（lwip sendto 不可重入） */
    if (xPortInIsrContext()) return 0;

    /* 互斥保护静态缓冲 */
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int len = vsnprintf(s_log_buf, sizeof(s_log_buf), fmt, args);
        if (len > 0) {
            sendto(s_udp_sock, s_log_buf, len, 0,
                   (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
        }
        xSemaphoreGive(s_log_mutex);
        return len;
    }
    return 0;
}

/* ── WiFi 连上后初始化 UDP 并检查崩溃历史 ── */
void wifi_log_init(void)
{
    s_log_mutex = xSemaphoreCreateMutex();

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family      = AF_INET;
    s_dest_addr.sin_port        = htons(4444);
    s_dest_addr.sin_addr.s_addr = INADDR_BROADCAST;

    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket 创建失败: errno=%d", errno);
        return;
    }
    int opt = 1;
    setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    s_orig_vprintf = esp_log_set_vprintf(udp_log_vprintf);

    ESP_LOGI(TAG, "===== WiFi UDP 日志已启动 → 广播 255.255.255.255:4444 =====");

    /* 检查上一次崩溃信息 */
    if (s_crash_store.magic == 0xDEADBEEF) {
        ESP_LOGE(TAG, "===== 上次崩溃 PC=0x%08X core=%lu cause=%lu =====",
                 (unsigned)s_crash_store.pc,
                 (unsigned long)s_crash_store.core,
                 (unsigned long)s_crash_store.cause);
        if (s_crash_store.bt_depth > 0) {
            char bt_buf[256];
            int off = 0;
            off += snprintf(bt_buf + off, sizeof(bt_buf) - off, "Backtrace:");
            for (int i = 0; i < s_crash_store.bt_depth && i < 8; i++) {
                off += snprintf(bt_buf + off, sizeof(bt_buf) - off,
                                " 0x%08X", (unsigned)s_crash_store.backtrace[i]);
            }
            ESP_LOGE(TAG, "%s", bt_buf);
        }
        /* 清除标记，避免重复上报 */
        s_crash_store.magic = 0;
    }
}

/* ── Panic 回调：在 CPU halt 前把崩溃信息写入 RTC RAM ── */
void wifi_log_panic_store(uint32_t pc, uint32_t core, uint32_t cause,
                          const uint32_t *bt, int bt_depth)
{
    s_crash_store.magic = 0xDEADBEEF;
    s_crash_store.pc    = pc;
    s_crash_store.core  = core;
    s_crash_store.cause = cause;
    s_crash_store.bt_depth = (bt_depth < 8) ? bt_depth : 8;
    for (int i = 0; i < s_crash_store.bt_depth; i++) {
        s_crash_store.backtrace[i] = bt[i];
    }
    /* RTC RAM 写入完成，系统随后会 reset */
}
