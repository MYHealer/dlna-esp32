/*
 * Now Playing — 薄接口层实现
 *
 * 只做元数据存储 + 回调通知，零 I/O、零内存分配。
 * 封面下载/解码由 dlna.c 的 album_art_task 处理。
 * 歌词获取由 dlna.c 的 lyrics_fetch_async 处理。
 *
 * 参考 FusionPlay：source 标签 + epoch 计数器，
 * 协议切换时递增 epoch，旧事件自动失效。
 */

#include "now_playing.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "NP";

/* ── 存储 ── */
static np_meta_t        s_meta;
/* cover 源缓冲：动态 PSRAM 分配（base64 封面可达几十 KB，不占内部 SRAM 挤爆 I2S DMA） */
#define NP_COVER_MAX 65536
static char            *s_cover_url = NULL;
static np_source_t      s_source = NP_SRC_NONE;
static uint32_t         s_epoch  = 0;

/* ── 回调 ── */
static np_meta_changed_cb_t  s_meta_cb         = NULL;
static np_cover_url_cb_t     s_cover_url_cb    = NULL;
static np_source_changed_cb_t s_source_cb      = NULL;

/* ═══════════════════════════════════════════════════════════════
 *  来源协议管理
 * ═══════════════════════════════════════════════════════════════ */

void np_set_source(np_source_t src)
{
    if (src == s_source) return;
    s_source = src;
    s_epoch++;
    ESP_LOGI(TAG, "Source changed → %d, epoch=%lu", src, (unsigned long)s_epoch);
    /* 协议切换 → 清除旧数据 */
    memset(&s_meta, 0, sizeof(s_meta));
    if (s_cover_url) { heap_caps_free(s_cover_url); s_cover_url = NULL; }
    if (s_source_cb) s_source_cb(src);
}

np_source_t np_get_source(void) { return s_source; }
uint32_t    np_get_epoch(void)  { return s_epoch; }

/* ═══════════════════════════════════════════════════════════════
 *  元数据
 * ═══════════════════════════════════════════════════════════════ */

void np_set_meta(const np_meta_t *meta)
{
    if (!meta) return;
    memcpy(&s_meta, meta, sizeof(s_meta));
    s_meta.epoch = s_epoch;  /* 打上当前 epoch 标记 */
    ESP_LOGI(TAG, "Meta set [%s] epoch=%lu: %.32s - %.24s (%lu ms)",
             s_source == NP_SRC_DLNA ? "DLNA" : s_source == NP_SRC_MIPLAY ? "MiPlay" : "?",
             (unsigned long)s_epoch,
             s_meta.title, s_meta.artist, (unsigned long)s_meta.duration_ms);
    if (s_meta_cb) s_meta_cb(&s_meta);
}

const np_meta_t *np_get_meta(void)
{
    return &s_meta;
}

/* ═══════════════════════════════════════════════════════════════
 *  统一提交（源 + 元数据 + 封面）
 * ═══════════════════════════════════════════════════════════════ */

void np_submit(np_source_t src, const np_meta_t *meta)
{
    if (!meta) return;
    if (src != s_source) {
        np_set_source(src);  /* 清旧数据 + epoch++ */
    }
    np_meta_t copy = *meta;
    copy.source = src;
    copy.epoch  = s_epoch;
    memcpy(&s_meta, &copy, sizeof(s_meta));
    ESP_LOGI(TAG, "Submit [%s] ep=%lu: %.32s - %.24s (%lums) cover=%d",
             src == NP_SRC_DLNA ? "DLNA" : src == NP_SRC_MIPLAY ? "MiPlay" : "?",
             (unsigned long)s_epoch,
             s_meta.title, s_meta.artist, (unsigned long)s_meta.duration_ms,
             (int)meta->has_cover);
    /* 先触发 meta 回调（UI 可 clear_cover + set_title），再触发 cover 回调 */
    if (s_meta_cb) s_meta_cb(&s_meta);
    if (meta->has_cover && meta->cover_url[0]) {
        np_set_cover_url(meta->cover_url);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  封面 URL（动态 PSRAM 缓冲，避免占内部 SRAM）
 * ═══════════════════════════════════════════════════════════════ */

static void np_cover_free(void)
{
    if (s_cover_url) {
        heap_caps_free(s_cover_url);
        s_cover_url = NULL;
    }
}

/* 内部：存 url 到 PSRAM（截断到 NP_COVER_MAX），返回 0 成功 */
static int np_cover_store(const char *url)
{
    if (!url) return -1;
    size_t len = strlen(url);
    if (len >= NP_COVER_MAX) len = NP_COVER_MAX - 1;
    char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return -1;
    np_cover_free();
    memcpy(buf, url, len);
    buf[len] = 0;
    s_cover_url = buf;
    return 0;
}

void np_set_cover_url(const char *url)
{
    if (!url || !url[0]) return;
    if (np_cover_store(url) != 0) return;
    ESP_LOGI(TAG, "Cover URL [%s]: %.80s",
             s_source == NP_SRC_DLNA ? "DLNA" : s_source == NP_SRC_MIPLAY ? "MiPlay" : "?",
             s_cover_url);
    if (s_cover_url_cb) s_cover_url_cb(s_cover_url);
}

const char *np_get_cover_url(void)
{
    return s_cover_url;
}

/* ═══════════════════════════════════════════════════════════════
 *  回调注册
 * ═══════════════════════════════════════════════════════════════ */

void np_set_meta_changed_cb(np_meta_changed_cb_t cb)   { s_meta_cb = cb; }
void np_set_cover_url_cb(np_cover_url_cb_t cb)         { s_cover_url_cb = cb; }
void np_set_source_changed_cb(np_source_changed_cb_t cb) { s_source_cb = cb; }

/* ═══════════════════════════════════════════════════════════════
 *  清除所有状态
 * ═══════════════════════════════════════════════════════════════ */

void np_clear(void)
{
    memset(&s_meta, 0, sizeof(s_meta));
    if (s_cover_url) { heap_caps_free(s_cover_url); s_cover_url = NULL; }
    ESP_LOGI(TAG, "State cleared (epoch=%lu)", (unsigned long)s_epoch);
}

/* ═══════════════════════════════════════════════════════════════
 *  初始化（零内存分配）
 * ═══════════════════════════════════════════════════════════════ */

esp_err_t np_init(void)
{
    memset(&s_meta, 0, sizeof(s_meta));
    if (s_cover_url) { heap_caps_free(s_cover_url); s_cover_url = NULL; }
    s_source = NP_SRC_NONE;
    s_epoch  = 0;
    ESP_LOGI(TAG, "Now Playing initialized (thin interface + epoch)");
    return ESP_OK;
}