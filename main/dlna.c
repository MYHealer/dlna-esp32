/* DLNA MediaRenderer — HTTP stream + ADF esp_audio playback
 *
 * 参考 airplay-esp32 的思路做了这些改进：
 *   - 播放状态机 + mutex，避免回调与 UI 的竞态
 *   - Stop/Seek/Next 时显式 flush pipeline（audio_element_stop_reset）
 *   - 加大 http_stream / i2s_stream ringbuffer，吸收网络抖动
 *   - HTTP 请求大小 request_size 调到 MSS 级别，减少小包开销
 *   - 软音量（vol/mute 缓存 + 事件通知），避免频繁 HAL 调用
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "board.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "esp_timer.h"

#include "audio_mem.h"
#include "esp_wifi.h"
#include "esp_audio.h"
#include "esp_decoder.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "media_lib_adapter.h"
#include "audio_idf_version.h"
#include "audio_element.h"
#include "ringbuf.h"

#include "custom_dlna.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "rotary_encoder.h"
#include "tft_display.h"
#include "lvgl_port.h"
#include "lyrics_fetch.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "audio_forge.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#define lodepng_malloc(s) heap_caps_malloc(s, MALLOC_CAP_SPIRAM)
#define lodepng_free(p)  heap_caps_free(p)
#include "extra/libs/png/lodepng.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "DLNA_APP";

/* ── Wrap audio_forge_init: 增大默认栈 4KB→8KB ── */
extern audio_element_handle_t __real_audio_forge_init(audio_forge_cfg_t *config);
audio_element_handle_t __wrap_audio_forge_init(audio_forge_cfg_t *config)
{
    if (config && config->task_stack < 8192) {
        ESP_LOGI(TAG, "Audio_forge: stack %d -> 8192", config->task_stack);
        config->task_stack = 8192;
    }
    return __real_audio_forge_init(config);
}

#define DLNA_DEVICE_UUID "8db0797a-f01a-4949-8f59-51188b18180b"

/* ─────────────────────── 播放状态机 ─────────────────────── */
typedef enum {
    PS_NO_MEDIA = -1,
    PS_STOPPED  = 0,
    PS_PLAYING  = 1,
    PS_PAUSED   = 2,
} play_state_t;

static esp_audio_handle_t player          = NULL;
static audio_element_handle_t s_http_el   = NULL;
static audio_element_handle_t s_i2s_el    = NULL;

static SemaphoreHandle_t s_state_mux     = NULL;
static play_state_t  s_state             = PS_STOPPED;
static char         *s_track_uri         = NULL;
static int           s_vol               = 35;
static int           s_mute              = 0;
/* 断点续播：记住暂停/停止时的播放位置 */
static int           s_saved_pos_sec     = 0;
static char         *s_saved_uri         = NULL;
/* 用于 GENA 去抖：相同状态不重复 notify */
static play_state_t  s_last_notified     = PS_STOPPED;
/* 宽限期：play() 后 8 秒内不被轮询覆盖 PLAYING 状态 */
static int64_t       s_grace_until       = 0;
/* Next URI 预设 */
static char         *s_next_uri          = NULL;
static char         *s_next_metadata     = NULL;
/* 用户主动停止标志 */
static int           s_user_stopped      = 0;
/* 位置追踪：纯软件方案（参考 miair-next） */
static int64_t        s_play_start_us     = 0;   /* 播放开始时间（esp_timer us），0=未在播放 */
static int            s_accumulated_ms    = 0;   /* 累计已播放 ms（暂停/停止时冻结） */


/* ── 工具：获取字符串形式的 DLNA 状态 ── */
static const char *state_str(play_state_t s)
{
    switch (s) {
        case PS_PLAYING:  return "PLAYING";
        case PS_PAUSED:   return "PAUSED_PLAYBACK";
        case PS_NO_MEDIA: return "NO_MEDIA_PRESENT";
        default:          return "STOPPED";
    }
}

/* ── 写状态（持锁）── */
static void set_state(play_state_t s)
{
    if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY);
    s_state = s;
    if (s_state_mux) xSemaphoreGive(s_state_mux);

    /* GENA: 只有真正的状态变迁才 notify (async: safe from esp_audio task) */
    if (s != s_last_notified) {
        s_last_notified = s;
        custom_dlna_notify_transport_state_async();
    }
}

/* ── 读状态（持锁）── */
static play_state_t get_state(void)
{
    play_state_t s;
    if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY);
    s = s_state;
    if (s_state_mux) xSemaphoreGive(s_state_mux);
    return s;
}

/* ── Delayed STOPPED notification (forward decl) ── */
static void delayed_stop_notify(void *arg);

/* ─────────────────────── Player event handler ───────────────────────
 * esp_audio 的底层事件 — 用来把我们自己的状态机和底层硬件对齐。
 * 注意：这里不能做耗时 I/O（比如 HTTP callback / GENA notify 内部 I/O
 * 会放在另外的 FreeRTOS task 里）。
 */
static void player_event_handler(esp_audio_state_t *state, void *ctx)
{
    (void)ctx;
    if (!state) return;

    ESP_LOGD(TAG, "player event: status=%d err=%d", state->status, state->err_msg);

    switch (state->status) {
        case AUDIO_STATUS_RUNNING:
            set_state(PS_PLAYING);
            /* 如果 play() 没设置追踪（非 cb_play 触发），在这里初始化 */
            if (s_play_start_us == 0) {
                s_play_start_us = esp_timer_get_time();
            }
            break;
        case AUDIO_STATUS_PAUSED:
            /* 只在用户主动 pause 时才切 PAUSED。
             * I2S 在采样率切换（audio_rate_follow_task 调 i2s_stream_set_clk）
             * 时会短暂进入 PAUSED，若无条件切状态会导致界面误显示"暂停"。 */
            if (s_user_stopped) set_state(PS_PAUSED);
            break;
        case AUDIO_STATUS_STOPPED:
            /* If we intentionally paused, ignore spurious STOPPED from esp_audio */
            if (get_state() != PS_PAUSED) set_state(PS_STOPPED);
            break;
        case AUDIO_STATUS_FINISHED:
            /* Same: don't let FINISHED clobber our PAUSED state */
            if (get_state() == PS_PAUSED) break;
            /* Don't notify STOPPED immediately — let I2S buffer drain first */
            xTaskCreatePinnedToCore(delayed_stop_notify, "stop_dly", 3072, NULL, 5, NULL, 1);
            break;
        case AUDIO_STATUS_ERROR:
            set_state(PS_STOPPED);
            break;
        default:
            break;
    }
}

/* ─────────────────────── DLNA 回调桥 ─────────────────────── */
static bool is_video_uri(const char *uri);  /* forward decl */

static const char *cb_get_transport_state(void) { return state_str(get_state()); }
static const char *cb_get_uri(void)             { return s_track_uri ? s_track_uri : ""; }

static int cb_get_position_sec(void)
{
    if (get_state() == PS_PLAYING && s_play_start_us > 0) {
        int elapsed_ms = (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        return (s_accumulated_ms + elapsed_ms) / 1000;
    }
    return s_accumulated_ms / 1000;
}

static int cb_get_position_ms(void)
{
    if (get_state() == PS_PLAYING && s_play_start_us > 0) {
        int elapsed_ms = (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        return s_accumulated_ms + elapsed_ms;
    }
    return s_accumulated_ms;
}

static int s_dur_cache_sec = 0;  /* 最近一次有效时长（秒） */

/* ── 当前歌曲信息（从 DIDL-Lite metadata 解析）── */
static char s_cur_title[128]  = "";
static char s_cur_artist[128] = "";

/* ── HTML 转义还原（DIDL-Lite 经过 HTML 编码）── */
static void html_unescape(char *dst, int dst_size, const char *src, int src_len)
{
    int di = 0;
    for (int i = 0; i < src_len && di < dst_size - 1; i++) {
        if (src[i] == '&') {
            if (strncmp(&src[i], "&amp;", 5) == 0)      { dst[di++] = '&';  i += 4; }
            else if (strncmp(&src[i], "&lt;", 4) == 0)   { dst[di++] = '<';  i += 3; }
            else if (strncmp(&src[i], "&gt;", 4) == 0)   { dst[di++] = '>';  i += 3; }
            else if (strncmp(&src[i], "&quot;", 6) == 0)  { dst[di++] = '"';  i += 5; }
            else if (strncmp(&src[i], "&apos;", 6) == 0)  { dst[di++] = '\''; i += 5; }
            else { dst[di++] = src[i]; }
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

/* 从 DIDL-Lite XML 中提取标签内容（HTML 转义版本） */
static int extract_tag(char *out, int out_size, const char *xml, const char *tag)
{
    /* 搜索 &lt;tagname...&gt; 或 &lt;tagname/&gt;（支持标签带属性） */
    char open_pat[48], close[48];
    snprintf(open_pat, sizeof(open_pat), "&lt;%s", tag);
    snprintf(close, sizeof(close), "&lt;/%s&gt;", tag);

    const char *p = strstr(xml, open_pat);
    if (!p) {
        /* 未转义形式 <tagname...> */
        char open2[32], close2[32];
        snprintf(open2, sizeof(open2), "<%s", tag);
        snprintf(close2, sizeof(close2), "</%s>", tag);
        p = strstr(xml, open2);
        if (!p) return -1;
        p += strlen(open2);
        while (*p && *p != '>' && !(*p == '/' && *(p+1) == '>')) p++;
        if (*p == '>') p++;
        else if (*p == '/') p += 2;
        if (!*p) return -1;
        const char *end = strstr(p, close2);
        if (!end) return -1;
        html_unescape(out, out_size, p, end - p);
        return 0;
    }
    /* 跳过 &lt;tagname，找到下一个 &gt; 或 > */
    p += strlen(open_pat);
    while (*p) {
        if (strncmp(p, "&gt;", 4) == 0) { p += 4; break; }
        if (*p == '>') { p++; break; }
        if (*p == '/' && *(p+1) == '>') { p += 2; break; }
        p++;
    }
    if (!*p) return -1;
    const char *end = strstr(p, close);
    if (!end) return -1;
    html_unescape(out, out_size, p, end - p);
    return 0;
}

static void parse_duration_from_metadata(const char *meta)
{
    if (!meta) return;
    /* 搜索 duration=&quot; （HTML 转义的引号） */
    const char *p = strstr(meta, "duration=&quot;");
    if (!p) {
        /* 也尝试未转义的形式 */
        p = strstr(meta, "duration=\"");
        if (!p) return;
        p += 10;
    } else {
        p += 15; /* skip 'duration=&quot;' */
    }
    int h = 0, m = 0, s = 0;
    if (sscanf(p, "%d:%d:%d", &h, &m, &s) >= 2) {
        int total = h * 3600 + m * 60 + s;
        if (total > 0) {
            s_dur_cache_sec = total;
            ESP_LOGI(TAG, "Parsed duration from metadata: %02d:%02d:%02d = %d sec",
                     h, m, s, total);
        }
    }
}

static int cb_get_duration_sec(void)
{
    play_state_t st = get_state();
    if (st == PS_STOPPED || st == PS_NO_MEDIA) return 0;
    /* 优先返回从 metadata 解析的时长（准确），
     * 不再依赖 esp_audio_duration_get（返回的是解码累计时间，不是歌曲时长） */
    return s_dur_cache_sec;
}

static int cb_get_volume(void) { return s_mute ? 0 : s_vol; }
static int cb_get_mute(void)   { return s_mute; }

/* ── 音量防抖：滑动时只应用最终值 ── */
static esp_timer_handle_t s_vol_debounce = NULL;
static int s_vol_pending = -1;

static void _vol_debounce_cb(void *arg)
{
    (void)arg;
    if (s_i2s_el && s_vol_pending >= 0) {
        int alc_gain = -(100 - s_vol_pending) * 30 / 100;
        i2s_alc_volume_set(s_i2s_el, alc_gain);
        ESP_LOGI(TAG, "vol=%d -> alc_gain=%d", s_vol_pending, alc_gain);
        s_vol_pending = -1;
    }
}

/* ── 自定义 vol_set：映射 0-100 到 ALC 增益（防抖）── */
static esp_err_t _my_vol_set(void *handle, int volume)
{
    if (!s_i2s_el) return ESP_ERR_INVALID_STATE;
    s_vol_pending = volume;
    if (!s_vol_debounce) {
        /* 首次调用时创建定时器 */
        const esp_timer_create_args_t args = {
            .callback = _vol_debounce_cb,
            .name = "vol_debounce",
        };
        esp_timer_create(&args, &s_vol_debounce);
    }
    /* 重置定时器：50ms 内无新值才真正应用 */
    esp_timer_stop(s_vol_debounce);
    esp_timer_start_once(s_vol_debounce, 50000);
    return ESP_OK;
}

static esp_err_t _my_vol_get(void *handle, int *volume)
{
    if (volume) *volume = s_vol;
    return ESP_OK;
}

/* ── 把音量 / 静音状态落到 ALC 并通知 GENA ── */
static void apply_volume_hw(void)
{
    int hw = s_mute ? 0 : s_vol;
    _my_vol_set(NULL, hw);
    custom_dlna_notify_rcs();
}

static void cb_set_uri(const char *uri)
{
    /* 视频过滤 */
    if (uri && is_video_uri(uri)) {
        ESP_LOGW(TAG, "Rejected video URI: %.128s", uri);
        return;
    }
    if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY);
    /* 新 URI 不同于保存的 URI 时，清除断点 */
    if (uri && s_saved_uri && strcmp(uri, s_saved_uri) != 0) {
        s_saved_pos_sec = 0;
        free(s_saved_uri);
        s_saved_uri = NULL;
    }
    free(s_track_uri);
    s_track_uri = uri ? strdup(uri) : NULL;
    /* 新 URI → 重置时长缓存 + 歌词 */
    s_dur_cache_sec = 0;
    s_cur_title[0] = '\0';
    s_cur_artist[0] = '\0';
    lyrics_clear();
    /* 状态转换：设置 URI 后从 NO_MEDIA → STOPPED */
    if (s_state == PS_NO_MEDIA && s_track_uri) {
        s_state = PS_STOPPED;
    }
    s_user_stopped = 0;
    if (s_state_mux) xSemaphoreGive(s_state_mux);
    ESP_LOGI(TAG, "SetURI: %s", s_track_uri ? s_track_uri : "(null)");
}

/* ── Delayed STOPPED notification: wait for I2S buffer to drain ── */
static void delayed_stop_notify(void *arg)
{
    (void)arg;
    /* 冻结最终位置 */
    if (s_play_start_us > 0) {
        s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        s_play_start_us = 0;
    }
    /* 立即通知 STOPPED，让 App 及时发送下一首。
     * （不能静默：网易云靠 STOPPED 事件才知道歌播完并推下一首） */
    set_state(PS_STOPPED);
    ESP_LOGI(TAG, "Track finished -> STOPPED (dur=%d ms)", s_accumulated_ms);
    /* 重置位置追踪为下一首做准备 */
    s_accumulated_ms = 0;
    vTaskDelete(NULL);
}

static void cb_play(void)
{
    play_state_t cur = get_state();
    ESP_LOGI(TAG, "Play (state=%d, saved_uri=%s, saved_pos=%d)",
             cur, s_saved_uri ? s_saved_uri : "(null)", s_saved_pos_sec);

    if (cur == PS_PAUSED && player) {
        /* 恢复暂停 — 优先用 esp_audio_resume */
        esp_audio_resume(player);
        set_state(PS_PLAYING);
        s_play_start_us = esp_timer_get_time();  /* 从当前时间继续计时 */
        s_grace_until = esp_timer_get_time() + 8000000LL;
        s_user_stopped = 0;
        ESP_LOGI(TAG, "Resumed from pause (pos=%d ms)", s_accumulated_ms);
    } else if (s_track_uri != NULL && player != NULL) {
        /* 新播放 or 断点续播 */
        bool same_track = s_saved_uri && s_track_uri
                          && strcmp(s_saved_uri, s_track_uri) == 0
                          && s_saved_pos_sec > 0;
        int seek_to = same_track ? s_saved_pos_sec : 0;

        esp_err_t err = esp_audio_play(player, AUDIO_CODEC_TYPE_DECODER, s_track_uri, 0);
        if (err == ESP_OK) {
            set_state(PS_PLAYING);
            s_accumulated_ms = seek_to * 1000;  /* 续播时从保存位置开始 */
            s_play_start_us = esp_timer_get_time();
            s_grace_until = esp_timer_get_time() + 8000000LL;
            s_user_stopped = 0;
            if (seek_to > 0) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_audio_seek(player, seek_to);
                ESP_LOGI(TAG, "Resumed from %d s (seek)", seek_to);
            }
        } else {
            ESP_LOGE(TAG, "esp_audio_play failed: 0x%x", err);
            set_state(PS_STOPPED);
        }
    }
    s_saved_pos_sec = 0;
}

static void cb_pause(void)
{
    play_state_t cur = get_state();
    ESP_LOGI(TAG, "Pause (state=%d)", cur);
    if (player && cur == PS_PLAYING) {
        /* 冻结累计播放时间 */
        if (s_play_start_us > 0) {
            s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
            s_play_start_us = 0;
        }
        s_saved_pos_sec = s_accumulated_ms / 1000;
        free(s_saved_uri);
        s_saved_uri = s_track_uri ? strdup(s_track_uri) : NULL;
        esp_err_t err = esp_audio_pause(player);
        set_state(PS_PAUSED);
        s_user_stopped = 1;
        ESP_LOGI(TAG, "Paused at %d ms (err=0x%x)", s_accumulated_ms, err);
    }
}

static void cb_stop(void)
{
    ESP_LOGI(TAG, "Stop");
    /* 冻结累计播放时间 */
    if (s_play_start_us > 0) {
        s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        s_play_start_us = 0;
    }
    s_saved_pos_sec = s_accumulated_ms / 1000;
    free(s_saved_uri);
    s_saved_uri = s_track_uri ? strdup(s_track_uri) : NULL;
    if (player) esp_audio_stop(player, TERMINATION_TYPE_NOW);
    set_state(PS_STOPPED);
    s_user_stopped = 1;
    free(s_next_uri); s_next_uri = NULL;
    free(s_next_metadata); s_next_metadata = NULL;
    ESP_LOGI(TAG, "Stopped at %d ms", s_accumulated_ms);
}

/* ── Seek 防抖：拖动进度条时 controller 连发多个 Seek，只执行最后一个 ── */
static void cb_seek(int seconds)
{
    ESP_LOGI(TAG, "Seek %d s", seconds);
    if (!player) return;
    esp_audio_seek(player, seconds);
    /* 更新位置追踪 */
    s_accumulated_ms = seconds * 1000;
    s_play_start_us = esp_timer_get_time();
}

static void cb_set_volume(int v)
{
    s_vol = v;
    if (s_mute) {
        /* 用户在静音状态下调音量 —— 顺手取消静音更自然 */
        s_mute = 0;
    }
    apply_volume_hw();
}

static void cb_set_mute(int m)
{
    s_mute = m ? 1 : 0;
    apply_volume_hw();
}

/* ── 视频过滤：检测 URI 是否指向视频文件 ── */
static bool is_video_uri(const char *uri)
{
    if (!uri) return false;
    const char *exts[] = { ".mp4", ".mov", ".avi", ".mkv", ".flv", ".wmv",
                           ".m4v", ".3gp", ".ts", ".mts", ".m2ts", NULL };
    /* 找到 URI 的路径部分（跳过 scheme://host） */
    const char *path = strstr(uri, "://");
    if (path) path += 3; else path = uri;
    path = strchr(path, '/');  /* 找到第一个 / */
    if (!path) return false;
    int plen = strlen(path);
    for (int i = 0; exts[i]; i++) {
        int elen = strlen(exts[i]);
        if (plen >= elen) {
            const char *tail = path + plen - elen;
            if (strncasecmp(tail, exts[i], elen) == 0) return true;
        }
    }
    if (strstr(uri, "video/")) return true;
    return false;
}

/* Next / Previous */
static void cb_next(void) {
    ESP_LOGI(TAG, "Next (next_uri=%s)", s_next_uri ? s_next_uri : "(null)");
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    if (s_next_uri && s_next_uri[0]) {
        /* 保存下一首 metadata 再切换，避免 update_uri 清空 DLNA 层 */
        char *next_meta = s_next_metadata;
        if (player) esp_audio_stop(player, TERMINATION_TYPE_NOW);
        free(s_track_uri);
        s_track_uri = s_next_uri;
        s_next_uri = NULL;
        s_next_metadata = NULL;
        if (player) {
            esp_audio_play(player, AUDIO_CODEC_TYPE_DECODER, s_track_uri, 0);
            set_state(PS_PLAYING);
            s_play_start_us = esp_timer_get_time();
        }
        /* 同步 URI + metadata 到 DLNA 层，GetPositionInfo/GetMediaInfo 才能正确返回 */
        custom_dlna_update_uri(s_track_uri, next_meta);
        free(next_meta);
    } else {
        if (player) esp_audio_stop(player, TERMINATION_TYPE_NOW);
        set_state(PS_STOPPED);
        custom_dlna_update_uri(NULL, NULL);
    }
    /* 强制通知 controller：切歌时状态可能一直是 PLAYING，
       set_state() 不会触发 notify，必须显式调用 */
    custom_dlna_notify_transport_state_async();
}
static void cb_previous(void) {
    ESP_LOGI(TAG, "Previous");
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    if (player && s_track_uri) {
        esp_audio_stop(player, TERMINATION_TYPE_NOW);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_audio_play(player, AUDIO_CODEC_TYPE_DECODER, s_track_uri, 0);
        set_state(PS_PLAYING);
        s_play_start_us = esp_timer_get_time();
    } else {
        if (player) esp_audio_stop(player, TERMINATION_TYPE_NOW);
        set_state(PS_STOPPED);
    }
    custom_dlna_notify_transport_state_async();
}

static void cb_set_next_uri(const char *uri, const char *metadata)
{
    ESP_LOGI(TAG, "SetNextURI: %s", uri ? uri : "(null)");
    free(s_next_uri);
    s_next_uri = uri ? strdup(uri) : NULL;
    free(s_next_metadata);
    s_next_metadata = metadata ? strdup(metadata) : NULL;
}

static void fetch_album_art_async(const char *url);

/* 简易 HTML 实体解码：将 &#xxxxx; 转为 UTF-8 */
static void decode_html_entities(char *dst, size_t dst_size, const char *src)
{
    size_t pos = 0;
    while (*src && pos + 6 < dst_size) {
        if (*src == '&' && *(src + 1) == '#') {
            src += 2; /* 跳过 &# */
            unsigned long codepoint = 0;
            while (*src >= '0' && *src <= '9') {
                codepoint = codepoint * 10 + (*src - '0');
                src++;
            }
            if (*src == ';') src++; /* 跳过 ; */
            /* 将 Unicode code point 编码为 UTF-8 */
            if (codepoint < 0x80) {
                dst[pos++] = codepoint;
            } else if (codepoint < 0x800) {
                dst[pos++] = 0xC0 | (codepoint >> 6);
                dst[pos++] = 0x80 | (codepoint & 0x3F);
            } else if (codepoint < 0x10000) {
                dst[pos++] = 0xE0 | (codepoint >> 12);
                dst[pos++] = 0x80 | ((codepoint >> 6) & 0x3F);
                dst[pos++] = 0x80 | (codepoint & 0x3F);
            } else {
                dst[pos++] = 0xF0 | (codepoint >> 18);
                dst[pos++] = 0x80 | ((codepoint >> 12) & 0x3F);
                dst[pos++] = 0x80 | ((codepoint >> 6) & 0x3F);
                dst[pos++] = 0x80 | (codepoint & 0x3F);
            }
        } else {
            dst[pos++] = *src;
            src++;
        }
    }
    dst[pos] = '\0';
}

static void cb_set_metadata(const char *metadata)
{
    ESP_LOGI(TAG, "Metadata: %s", metadata);
    parse_duration_from_metadata(metadata);

    char title[256] = "", artist[256] = "", album_art[256] = "";
    extract_tag(title, sizeof(title), metadata, "dc:title");
    extract_tag(artist, sizeof(artist), metadata, "upnp:artist");
    extract_tag(album_art, sizeof(album_art), metadata, "upnp:albumArtURI");
    ESP_LOGI(TAG, "Extracted: title='%s' artist='%s' album_art='%s'", title, artist, album_art);

    if (album_art[0]) {
        /* 没有参数时追加 ?param=200y200 让 CDN 返回小图，节省内存 */
        if (!strchr(album_art, '?')) {
            strncat(album_art, "?param=300y300", sizeof(album_art) - strlen(album_art) - 1);
        }
        ESP_LOGI(TAG, "AlbumArt URL: %s", album_art);
        fetch_album_art_async(album_art);
    }

    if (title[0] && (strcmp(title, s_cur_title) != 0 || strcmp(artist, s_cur_artist) != 0)) {
        decode_html_entities(s_cur_title, sizeof(s_cur_title), title);
        decode_html_entities(s_cur_artist, sizeof(s_cur_artist), artist);
        ESP_LOGI(TAG, "Song: %s - %s", s_cur_title, s_cur_artist);
        lvgl_port_lock();
        lvgl_port_ui_set_title(s_cur_title);
        lvgl_port_ui_set_artist(s_cur_artist);
        lvgl_port_unlock();
        lyrics_fetch_async(s_cur_title, s_cur_artist);
    }
}

/* ─────────────────────── 专辑封面下载与解码 ─────────────────────── */

/* ── 用原始 socket 下载封面，全部走 PSRAM，避免 esp_http_client 争抢内部 RAM ── */
static int simple_http_get(const char *url, uint8_t **out_data, int *out_len)
{
    /* 解析 URL: http://host[:port]/path */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else { ESP_LOGW(TAG, "Only http supported"); return -1; }

    char host[128] = "";
    int port = 80;
    const char *path = "/";
    /* 提取 host */
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < 127) host[i++] = *p++;
    host[i] = '\0';
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') path = p;

    /* DNS 解析 */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS failed: %s", host);
        return -1;
    }

    /* 创建 socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    /* 设置超时 */
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock); freeaddrinfo(res);
        ESP_LOGW(TAG, "TCP connect failed: %s:%d", host, port);
        return -1;
    }
    freeaddrinfo(res);

    /* 发送 HTTP GET */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(sock, req, req_len, 0);

    /* 读取响应到 PSRAM 缓冲区（动态扩容，确保完整下载） */
    int cap = 256 * 1024;  /* 初始 256KB，足够大图 */
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { close(sock); return -1; }

    int total = 0, hdr_end = 0;
    for (;;) {
        int r = recv(sock, buf + total, cap - total - 1, 0);
        if (r <= 0) break;  /* 0=对端关闭, <0=错误 */
        total += r;
        /* 查找 HTTP 头结束标记 */
        if (!hdr_end) {
            for (int j = 0; j < total - 3; j++) {
                if (buf[j] == '\r' && buf[j+1] == '\n' && buf[j+2] == '\r' && buf[j+3] == '\n') {
                    hdr_end = j + 4;
                    break;
                }
            }
        }
        /* 缓冲区将满时扩容（PSRAM 有足够空间） */
        if (total > cap - 4096) {
            int new_cap = cap + 128 * 1024;
            uint8_t *new_buf = heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!new_buf) break;  /* 扩容失败，用已有数据 */
            buf = new_buf;
            cap = new_cap;
        }
    }
    close(sock);

    if (!hdr_end || total - hdr_end < 4) {
        ESP_LOGW(TAG, "HTTP response too small: %d (hdr_end=%d)", total, hdr_end);
        heap_caps_free(buf);
        return -1;
    }

    int body_len = total - hdr_end;
    ESP_LOGI(TAG, "HTTP response: total=%d, headers=%d, body=%d", total, hdr_end, body_len);
    /* 分配干净的 body 缓冲区（PSRAM） */
    uint8_t *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) { heap_caps_free(buf); return -1; }
    memcpy(body, buf + hdr_end, body_len);
    heap_caps_free(buf);

    *out_data = body;
    *out_len = body_len;
    ESP_LOGI(TAG, "Cover downloaded: %d bytes", body_len);
    return 0;
}

static void album_art_task(void *arg)
{
    const char *url = (const char *)arg;
    if (!url) { free(arg); vTaskDelete(NULL); return; }

    /* 等音频管线稳定 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint8_t *img_data = NULL;
    int img_len = 0;
    if (simple_http_get(url, &img_data, &img_len) != 0) {
        ESP_LOGW(TAG, "Cover download failed");
        free(arg);
        vTaskDelete(NULL);
        return;
    }

    /* 检测图片格式并解码 */
    ESP_LOGI(TAG, "Image data: len=%d, first4=%02X %02X %02X %02X",
             img_len, img_data[0], img_data[1], img_data[2], img_data[3]);
    int out_w = 0, out_h = 0;
    uint16_t *out_buf = NULL;
    bool jpeg_aligned = false;  /* JPEG 用 jpeg_calloc_align 分配 */

    if (img_data[0] == 0xFF && img_data[1] == 0xD8) {
        /* ====== JPEG 解码（esp_new_jpeg，支持 progressive） ====== */
        jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
        dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;  /* 小端序，CPU 直接读 */
        jpeg_dec_handle_t jpeg_dec = NULL;
        jpeg_error_t jerr = jpeg_dec_open(&dec_cfg, &jpeg_dec);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG open failed: %d", jerr);
            heap_caps_free(img_data); free(arg); vTaskDelete(NULL); return;
        }

        /* 直接解码全尺寸（DEFAULT_JPEG_DEC_CONFIG 已禁用 scale） */

        jpeg_dec_io_t io = { .inbuf = img_data, .inbuf_len = img_len };
        jpeg_dec_header_info_t info;
        jerr = jpeg_dec_parse_header(jpeg_dec, &io, &info);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG header parse failed: %d", jerr);
            jpeg_dec_close(jpeg_dec);
            heap_caps_free(img_data); free(arg); vTaskDelete(NULL); return;
        }
        out_w = info.width;
        out_h = info.height;

        int outbuf_len = 0;
        jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
        uint8_t *raw_buf = (uint8_t *)jpeg_calloc_align(outbuf_len, 16);
        if (!raw_buf) {
            ESP_LOGW(TAG, "JPEG outbuf alloc failed (%d)", outbuf_len);
            jpeg_dec_close(jpeg_dec);
            heap_caps_free(img_data); free(arg); vTaskDelete(NULL); return;
        }

        /* 关键：parse_header 已消费头部字节，inbuf_remain 是剩余未用字节。
           必须把 inbuf 指针前移到 SOS 段位置再交给 process 解码 */
        int consumed = io.inbuf_len - io.inbuf_remain;
        io.outbuf = raw_buf;
        io.out_size = outbuf_len;
        io.inbuf = img_data + consumed;
        io.inbuf_len = io.inbuf_remain;
        jerr = jpeg_dec_process(jpeg_dec, &io);
        jpeg_dec_close(jpeg_dec);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG decode failed: %d", jerr);
            jpeg_free_align(raw_buf);
            heap_caps_free(img_data); free(arg); vTaskDelete(NULL); return;
        }

        /* BGR565_BE + 中心裁剪 + 双线性缩放到 48x48 */
        int min_dim = out_w < out_h ? out_w : out_h;
        int crop_sz = min_dim;
        int crop_x = (out_w - crop_sz) / 2;
        int crop_y = (out_h - crop_sz) / 2;
        #define JPEG_MAX_COVER 48
        int sw = JPEG_MAX_COVER, sh = JPEG_MAX_COVER;
        uint16_t *resized = (uint16_t *)heap_caps_malloc(sw * sh * 2, MALLOC_CAP_SPIRAM);
        if (!resized) {
            ESP_LOGW(TAG, "JPEG resize alloc failed");
            jpeg_free_align(raw_buf);
            heap_caps_free(img_data); free(arg); vTaskDelete(NULL); return;
        }
        uint16_t *src = (uint16_t *)raw_buf;
        for (int y = 0; y < sh; y++) {
            float fy = crop_y + (float)y / sh * crop_sz;
            for (int x = 0; x < sw; x++) {
                float fx = crop_x + (float)x / sw * crop_sz;
                int ix = (int)fx, iy = (int)fy;
                if (ix >= out_w - 1) ix = out_w - 2;
                if (iy >= out_h - 1) iy = out_h - 2;
                float dx = fx - ix, dy = fy - iy;
                /* 四个邻近像素（RGB565_BE，已含 BGR 交换前的原始数据） */
                uint16_t p00 = src[iy * out_w + ix];
                uint16_t p01 = src[iy * out_w + ix + 1];
                uint16_t p10 = src[(iy + 1) * out_w + ix];
                uint16_t p11 = src[(iy + 1) * out_w + ix + 1];
                /* 分通道双线性插值（RGB565: R=5bit, G=6bit, B=5bit） */
                int r = (int)((((p00>>11)&0x1F)*(1-dx) + ((p01>>11)&0x1F)*dx)*(1-dy) +
                              (((p10>>11)&0x1F)*(1-dx) + ((p11>>11)&0x1F)*dx)*dy);
                int g = (int)((((p00>>5)&0x3F)*(1-dx) + ((p01>>5)&0x3F)*dx)*(1-dy) +
                              (((p10>>5)&0x3F)*(1-dx) + ((p11>>5)&0x3F)*dx)*dy);
                int b = (int)(((p00&0x1F)*(1-dx) + (p01&0x1F)*dx)*(1-dy) +
                              ((p10&0x1F)*(1-dx) + (p11&0x1F)*dx)*dy);
                /* BGR565 大端序（ST7735 MADCTL_BGR） */
                uint16_t c = ((b & 0x1F) << 11) | ((g & 0x3F) << 5) | (r & 0x1F);
                resized[y * sw + x] = (c >> 8) | (c << 8);
            }
        }
        jpeg_free_align(raw_buf);
        out_buf = resized;
        jpeg_aligned = false;  /* resized 用 heap_caps_malloc，不用 jpeg_free_align */
        out_w = sw; out_h = sh;
        ESP_LOGI(TAG, "JPEG decoded: %dx%d -> crop %d -> %dx%d", info.width, info.height, crop_sz, sw, sh);

    } else if (img_data[0] == 0x89 && img_data[1] == 0x50) {
        /* ====== PNG 解码（lodepng） ====== */
        unsigned png_w = 0, png_h = 0;
        uint8_t *png_rgb = NULL;
        /* 用 RGB（3 字节/像素）而非 RGBA，省 25% 内存 */
        unsigned err = lodepng_decode24(&png_rgb, &png_w, &png_h, img_data, img_len);
        if (err) {
            ESP_LOGW(TAG, "PNG decode failed: %u (%s), w=%u h=%u", err, lodepng_error_text(err), png_w, png_h);
            heap_caps_free(img_data);
            free(arg);
            vTaskDelete(NULL);
            return;
        }
        /* 下载缓冲区已无用，立即释放 */
        heap_caps_free(img_data);
        img_data = NULL;

        /* 中心裁剪 + 缩放到 48x48 */
        int min_dim = png_w < png_h ? png_w : png_h;
        int crop_sz = min_dim;
        int crop_x = (png_w - crop_sz) / 2;
        int crop_y = (png_h - crop_sz) / 2;
        #define MAX_COVER 48
        int sw = MAX_COVER, sh = MAX_COVER;

        size_t out_buf_size = sw * sh * 2;
        out_buf = (uint16_t *)heap_caps_malloc(out_buf_size, MALLOC_CAP_SPIRAM);
        if (!out_buf) {
            ESP_LOGW(TAG, "PNG out_buf alloc failed (%zu)", out_buf_size);
            free(png_rgb);
            free(arg);
            vTaskDelete(NULL);
            return;
        }
        /* 双线性缩放 + RGB888→BGR565 */
        for (int y = 0; y < sh; y++) {
            float fy = crop_y + (float)y / sh * crop_sz;
            for (int x = 0; x < sw; x++) {
                float fx = crop_x + (float)x / sw * crop_sz;
                int ix = (int)fx, iy = (int)fy;
                if (ix >= png_w - 1) ix = png_w - 2;
                if (iy >= png_h - 1) iy = png_h - 2;
                float dx = fx - ix, dy = fy - iy;
                int base = iy * png_w + ix;
                int r = (int)((png_rgb[base*3]*(1-dx) + png_rgb[(base+1)*3]*dx)*(1-dy) +
                              (png_rgb[(base+png_w)*3]*(1-dx) + png_rgb[(base+png_w+1)*3]*dx)*dy);
                int g = (int)((png_rgb[base*3+1]*(1-dx) + png_rgb[(base+1)*3+1]*dx)*(1-dy) +
                              (png_rgb[(base+png_w)*3+1]*(1-dx) + png_rgb[(base+png_w+1)*3+1]*dx)*dy);
                int b = (int)((png_rgb[base*3+2]*(1-dx) + png_rgb[(base+1)*3+2]*dx)*(1-dy) +
                              (png_rgb[(base+png_w)*3+2]*(1-dx) + png_rgb[(base+png_w+1)*3+2]*dx)*dy);
                uint16_t c = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
                out_buf[y * sw + x] = (c >> 8) | (c << 8);
            }
        }
        free(png_rgb);
        out_w = sw; out_h = sh;
        ESP_LOGI(TAG, "PNG decoded: %ux%u -> %dx%d (crop=%d)", png_w, png_h, out_w, out_h, crop_sz);

    } else {
        ESP_LOGW(TAG, "Unknown format: %02X %02X %02X %02X",
                 img_data[0], img_data[1], img_data[2], img_data[3]);
        heap_caps_free(img_data);
        free(arg);
        vTaskDelete(NULL);
        return;
    }

    /* 显示封面（数据通过 memcpy 到内部 RAM 静态缓冲区，无缓存问题） */
    lvgl_port_lock();
    lvgl_port_ui_set_cover(out_buf, out_w, out_h);
    lvgl_port_unlock();

    /* 清理 */
    if (jpeg_aligned) jpeg_free_align(out_buf);
    else heap_caps_free(out_buf);
    heap_caps_free(img_data);
    free(arg);
    vTaskDelete(NULL);
}

static void fetch_album_art_async(const char *url)
{
    char *url_copy = strdup(url);
    if (!url_copy) return;
    /* Core 1: 与 WiFi (Core 0) 完全隔离，避免争抢内部 RAM */
    BaseType_t ret = xTaskCreatePinnedToCore(album_art_task, "album_art",
        8 * 1024, url_copy, 1, NULL, 1);  /* 最低优先级，避免与音频管线争抢 CPU */
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "album_art task create failed: %d", ret);
        free(url_copy);
    }
}

/* ─────────────────────── HTTP stream event ─────────────────────── */
static int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    if (!msg) return ESP_OK;
    switch (msg->event_id) {
        case HTTP_STREAM_RESOLVE_ALL_TRACKS:
            return ESP_OK;
        case HTTP_STREAM_FINISH_TRACK:
            return http_stream_next_track(msg->el);
        case HTTP_STREAM_FINISH_PLAYLIST:
            return http_stream_restart(msg->el);
        default:
            return ESP_OK;
    }
}

/* ─────────────────────── Audio player init ─────────────────────── */
static void audio_player_init(void)
{
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal,
                         AUDIO_HAL_CODEC_MODE_DECODE,
                         AUDIO_HAL_CTRL_START);

    /* ── esp_audio 主体
     *    N16R8 有 8MB PSRAM，大胆用！
     *    prefer_type = SPEED：优先性能，减少解码延迟
     *    in_stream_buf = 512KB：HTTP→Decoder 缓冲，减少网络抖动
     *    out_stream_buf = 128KB：Decoder→I2S 缓冲，防止 underrun
     */
    esp_audio_cfg_t cfg = DEFAULT_ESP_AUDIO_CONFIG();
    cfg.vol_handle = board_handle->audio_hal;
    cfg.vol_set = (audio_volume_set)audio_hal_set_volume;
    cfg.vol_get = (audio_volume_get)audio_hal_get_volume;
    cfg.prefer_type  = ESP_AUDIO_PREFER_SPEED;
    cfg.resample_rate = 0;           /* 0=禁用重采样，I2S 跟随源采样率（参考 Jw-Y1）*/
    cfg.in_stream_buf_size  = 512 * 1024;  /* 512KB，充分吸收网络抖动 */
    cfg.out_stream_buf_size = 128 * 1024;  /* 128KB，解码后充裕缓冲 */
    cfg.task_prio   = 8;
    cfg.task_stack  = 8 * 1024;
    player = esp_audio_create(&cfg);

    /* ── HTTP stream reader
     *    out_rb = 1MB：PSRAM 大缓冲，预加载整首歌的很大一部分
     *    request_size = 32KB：大块读取，减少 HTTP RTT
     *    user_agent = 浏览器 UA：网易云对非浏览器 UA 会限速/给低码率
     *                 （参考 Jw-Y1 + miair-next 的 UA 伪装）
     *    stack_in_ext = true：任务栈放 PSRAM，节省 IRAM
     */
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.event_handle      = _http_stream_event_handle;
    http_cfg.type              = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
    http_cfg.task_prio         = 6;
    http_cfg.task_stack        = 8 * 1024;
    http_cfg.stack_in_ext      = true;
    http_cfg.out_rb_size       = 1024 * 1024;  /* 1MB PSRAM 环形缓冲 */
    http_cfg.request_size      = 32768;         /* 32KB 请求块，减少 RTT */
    http_cfg.user_agent        = "Mozilla/5.0 (Linux; Android 12) "
                                 "AppleWebKit/537.36 (KHTML, like Gecko) "
                                 "Chrome/120.0.0.0 Mobile Safari/537.36";
    s_http_el = http_stream_init(&http_cfg);
    esp_audio_input_stream_add(player, s_http_el);

    /* ── Decoders（mp3/aac/wav/ts...）── */
    audio_decoder_t auto_decode[] = {
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_M4A_DECODER_CONFIG(),
        DEFAULT_ESP_TS_DECODER_CONFIG(),
    };
    esp_decoder_cfg_t auto_dec_cfg = DEFAULT_ESP_DECODER_CONFIG();
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER,
                            esp_decoder_init(&auto_dec_cfg, auto_decode, 5));

    /* ── I2S stream writer
     *    out_rb = 256KB：PSRAM 缓冲，彻底杜绝 DMA underrun
     *    task_core = 1：与 WiFi（core0）分开，避免竞争
     *    task_prio = 9：最高优先级，及时喂 DMA
     */
    i2s_stream_cfg_t i2s_writer = I2S_STREAM_CFG_DEFAULT();
    i2s_writer.type             = AUDIO_STREAM_WRITER;
    i2s_writer.stack_in_ext     = true;
    i2s_writer.task_prio        = 9;
    i2s_writer.task_core        = 1;
    i2s_writer.task_stack       = 4 * 1024;
    i2s_writer.out_rb_size      = 256 * 1024;  /* 256KB PSRAM，杜绝 underrun */
    i2s_writer.use_alc          = true;
    s_i2s_el = i2s_stream_init(&i2s_writer);
    i2s_stream_set_clk(s_i2s_el, 48000, 16, 2);
    esp_audio_output_stream_add(player, s_i2s_el);

    /* ── 起手音量 ── */
    _my_vol_set(NULL, s_vol);
    esp_audio_callback_set(player, player_event_handler, NULL);

    ESP_LOGI(TAG, "Audio player ready (http_rb=%d, i2s_rb=%d, req=%d, in_buf=%d, out_buf=%d)",
             http_cfg.out_rb_size, i2s_writer.out_rb_size, http_cfg.request_size,
             cfg.in_stream_buf_size, cfg.out_stream_buf_size);
}

/* ─────────────────────── 采样率跟随 ───────────────────────
 * 参考 Jw-Y1: I2S 跟随源采样率，不做强制重采样（省 CPU、保音质）。
 * 独立 task 周期读取 esp_audio 音乐信息，检测到采样率变化时更新 I2S clk。
 */
static void audio_rate_follow_task(void *arg)
{
    (void)arg;
    esp_audio_music_info_t info;
    int last_rate = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!player || !s_i2s_el) continue;
        if (get_state() != PS_PLAYING) continue;
        if (esp_audio_music_info_get(player, &info) == ESP_ERR_AUDIO_NO_ERROR) {
            if (info.sample_rates > 0 && info.sample_rates != last_rate) {
                ESP_LOGI(TAG, "I2S rate follow: %d -> %d Hz (%dch)",
                         last_rate, info.sample_rates, info.channels);
                int ch = (info.channels == 1) ? 1 : 2;
                i2s_stream_set_clk(s_i2s_el, info.sample_rates, 16, ch);
                last_rate = info.sample_rates;
            }
        }
    }
}

/* ─────────────────────── Rotary encoder ─────────────────────── */
static void _enc_on_btn(void *arg)
{
    (void)arg;
    play_state_t cur = get_state();
    if (cur == PS_PLAYING) {
        cb_pause();
    } else if (cur == PS_PAUSED) {
        cb_play();
    } else if (s_track_uri != NULL) {
        cb_play();
    }
}

static void _enc_on_rotate(void *arg, int direction)
{
    (void)arg;
    int step = 5;
    int new_vol = s_vol + (direction > 0 ? step : -step);
    if (new_vol < 0)   new_vol = 0;
    if (new_vol > 100) new_vol = 100;
    cb_set_volume(new_vol);
    ESP_LOGI(TAG, "Volume -> %d", new_vol);
}

static void rotary_encoder_setup(void)
{
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return;
    }
    static const rotary_encoder_config_t cfg = {
        .clk_gpio     = 4,
        .dt_gpio      = 5,
        .sw_gpio      = 6,
        .vol_step     = 5,
        .on_rotate    = _enc_on_rotate,
        .on_btn_click = _enc_on_btn,
        .arg          = NULL,
    };
    ret = rotary_encoder_init(&cfg);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "rotary_encoder_init failed: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Rotary encoder ready (CLK=4, DT=5, SW=6)");
}

/* ─────────────────────── Start DLNA ─────────────────────── */
static void start_dlna(void)
{
    static const custom_dlna_config_t cfg = {
        .friendly_name     = "MS01B",
        .uuid              = DLNA_DEVICE_UUID,
        .port              = 8080,

        .get_transport_state = cb_get_transport_state,
        .get_uri             = cb_get_uri,
        .get_position_sec    = cb_get_position_sec,
        .get_position_ms     = cb_get_position_ms,
        .get_duration_sec    = cb_get_duration_sec,
        .get_volume          = cb_get_volume,
        .get_mute            = cb_get_mute,

        .on_set_uri       = cb_set_uri,
        .on_set_next_uri   = cb_set_next_uri,
        .on_set_metadata   = cb_set_metadata,
        .on_play           = cb_play,
        .on_pause          = cb_pause,
        .on_stop           = cb_stop,
        .on_seek           = cb_seek,
        .on_set_volume     = cb_set_volume,
        .on_set_mute       = cb_set_mute,
        .on_next           = cb_next,
        .on_previous       = cb_previous,
    };
    custom_dlna_init(&cfg);
    ESP_LOGI(TAG, "Custom DLNA started (host=ESP32-S3-DLNA)");
}

/* ─────────────────────── LVGL UI 更新任务 ── */

/* 获取当前播放位置（毫秒） */
static int get_position_ms(void)
{
    if (s_play_start_us > 0) {
        return s_accumulated_ms +
               (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
    }
    return s_accumulated_ms;
}

static void ui_update_task(void *arg)
{
    (void)arg;
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        lvgl_port_lock();
        int pos_sec = get_position_ms() / 1000;
        ESP_LOGI(TAG, "UI: pos=%d dur=%d state=%d", pos_sec, s_dur_cache_sec, (int)get_state());
        lvgl_port_ui_set_progress(pos_sec, s_dur_cache_sec);
        lvgl_port_ui_set_state((int)get_state());
        lvgl_port_ui_set_volume(s_vol);

        const lyric_data_t *lyr = lyrics_get_data();
        if (lyr && lyr->loaded && lyr->count > 0) {
            int pos_ms = get_position_ms();
            int cur = lyrics_get_current_line(pos_ms);
            int start = cur - 2;
            if (start < 0) start = 0;
            if (start + 5 > lyr->count) start = lyr->count - 5;
            if (start < 0) start = 0;
            int act = cur - start;
            if (act < 0 || act >= 5) act = -1;
            for (int i = 0; i < 5; i++) {
                int src = start + i;
                lvgl_port_ui_set_lyric_line(i, (src < lyr->count) ? lyr->lines[src].text : "");
            }
            lvgl_port_ui_set_lyric_current(act);
        } else {
            for (int i = 0; i < 5; i++) lvgl_port_ui_set_lyric_line(i, "");
            lvgl_port_ui_set_lyric_current(-1);
        }
        lvgl_port_unlock();

        if ((++tick % 20) == 0) ESP_LOGI(TAG, "UI tick alive");
    }
}

/* ─────────────────────── Entry point ─────────────────────── */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("ESP_AUDIO_CTRL", ESP_LOG_WARN);
    /* 诊断卡顿：临时提高 HTTP/解码/内存日志级别 */
    esp_log_level_set("HTTP_STREAM", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_AUDIO_TASK", ESP_LOG_DEBUG);
    esp_log_level_set("esp_http_client", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("LYRIC", ESP_LOG_DEBUG);
    media_lib_add_default_adapter();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    /* ── 状态互斥锁（必须在其他组件之前创建）── */
    s_state_mux = xSemaphoreCreateMutex();

    /* ── WiFi
     *   关键点：关闭省电模式 + 固定主机名，从 airplay-esp32 参考过来。
     */
    {
        esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
        esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

        periph_wifi_cfg_t wifi_cfg = {
            .wifi_config.sta.ssid = CONFIG_WIFI_SSID,
            .wifi_config.sta.password = CONFIG_WIFI_PASSWORD,
            .wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .wifi_config.sta.pmf_cfg.capable = false,
            .wifi_config.sta.pmf_cfg.required = false,
            .wifi_config.sta.listen_interval = 1,
            .reconnect_timeout_ms = 5000,
            .disable_auto_reconnect = false,
        };
        esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
        esp_periph_start(set, wifi_handle);
        periph_wifi_wait_for_connected(wifi_handle, pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "WiFi %s", periph_wifi_is_connected(wifi_handle) ? "connected" : "timeout, continuing...");
    }

    /* ── 主机名 + WiFi 协议/功耗
     *   路由器 UI / mDNS / 网络邻居 里就能看到 "ESP32-S3-DLNA"。
     */
    {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) esp_netif_set_hostname(sta_netif, "ESP32-S3-DLNA");
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(ESP_IF_WIFI_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_max_tx_power(78);

    /* ── 音频播放 ── */
    audio_player_init();

    /* ── 采样率跟随（I2S 动态匹配源采样率，core1）── */
    xTaskCreatePinnedToCore(audio_rate_follow_task, "rate_follow", 4096, NULL, 4, NULL, 1);

    /* ── DLNA 服务（SSDP + HTTP + SOAP）── */
    start_dlna();

    /* ── 旋钮（UI）── */
    rotary_encoder_setup();

    /* ── TFT 显示 + LVGL ── */
    tft_init();
    lvgl_port_init(5);
    lyrics_init();
    xTaskCreatePinnedToCore(ui_update_task, "ui_update", 4096, NULL, 3, NULL, 0);
}
