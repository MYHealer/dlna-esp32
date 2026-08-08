/* DLNA MediaRenderer — GMF pipeline 播放
 *
 * 从 ESP-ADF 迁移到 ESP-GMF，保留 DLNA 协议逻辑不变。
 * 音频管线：io_http → aud_dec → aud_alc → aud_ch_cvt → aud_bit_cvt → io_codec_dev
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

/* GMF 音频框架 */
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_alc.h"
#include "esp_gmf_event.h"
#include "esp_gmf_info.h"
#include "esp_gmf_audio_element.h"
#include "gmf_loader_setup_defaults.h"

#include "custom_dlna.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "rotary_encoder.h"
#include "tft_display.h"
#include "lvgl_port.h"
#include "lyrics_fetch.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#define lodepng_malloc(s) heap_caps_malloc(s, MALLOC_CAP_SPIRAM)
#define lodepng_free(p)  heap_caps_free(p)
#include "extra/libs/png/lodepng.h"

static const char *TAG = "DLNA_APP";

#define DLNA_DEVICE_UUID "8db0797a-f01a-4949-8f59-51188b18180b"

/* ─────────────────────── 播放状态机 ─────────────────────── */
typedef enum {
    PS_NO_MEDIA = -1,
    PS_STOPPED  = 0,
    PS_PLAYING  = 1,
    PS_PAUSED   = 2,
} play_state_t;

/* GMF 音频管线 */
static esp_gmf_pipeline_handle_t s_pipe       = NULL;
static esp_gmf_task_handle_t     s_work_task  = NULL;
static esp_codec_dev_handle_t    s_codec_dev   = NULL;
static esp_gmf_element_handle_t  s_dec_el     = NULL;
static esp_gmf_element_handle_t  s_alc_el     = NULL;
static esp_gmf_pool_handle_t     s_pool       = NULL;

static SemaphoreHandle_t s_state_mux     = NULL;
static play_state_t  s_state             = PS_STOPPED;
static char         *s_track_uri         = NULL;
static int           s_vol               = 35;
static int           s_last_applied_vol  = 35;  /* 上次实际应用到 ALC 的音量 */
static int           s_mute              = 0;
/* I2S 时钟跟踪：避免重复重配 */
static int           s_last_i2s_rate     = 48000;
static int           s_last_i2s_bits     = 16;
static int           s_last_i2s_ch       = 2;
/* 断点续播：记住暂停/停止时的播放位置 */
static int           s_saved_pos_sec     = 0;
static char         *s_saved_uri         = NULL;
static unsigned long s_music_id          = 0;   /* 当前歌曲 ID，用于检测音质切换 */
/* 用于 GENA 去抖：相同状态不重复 notify */
static play_state_t  s_last_notified     = PS_STOPPED;
/* 宽限期：play() 后 8 秒内不被轮询覆盖 PLAYING 状态 */
static int64_t       s_grace_until       = 0;
/* Next URI 预设 */
static char         *s_next_uri          = NULL;
static char         *s_next_metadata     = NULL;
/* 用户主动停止标志 */
static int           s_user_stopped      = 0;
/* 歌词 UI 清除请求（新歌切歌时，UI 任务处理） */
static volatile bool s_lyrics_ui_clear_pending = false;
/* 位置追踪：纯软件方案（参考 miair-next） */
static int64_t        s_play_start_us     = 0;   /* 播放开始时间（esp_timer us），0=未在播放 */
static int            s_accumulated_ms    = 0;   /* 累计已播放 ms（暂停/停止时冻结） */
static char          *s_playing_uri       = NULL; /* 当前音频管道实际加载的 URI，用于检测暂停时切歌 */
/* 主动播完检测（参考 miair-next _check_play_status）：
 * 软件位置接近曲末但底层未触发 FINISHED 时的兜底，避免控制器等不到 STOPPED 不切歌 */
static int            s_near_end_count    = 0;   /* 连续接近曲末的 tick 数 */
static bool           s_finish_notify_spawned = false; /* 防重复触发播完处理 */


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
static void cb_next(void);
static void cb_previous(void);
static void cb_play_toggle(void);

/* ─────────────────────── GMF pipeline 事件回调 ─────────────────────── */
static esp_gmf_err_t pipeline_event_cb(esp_gmf_event_pkt_t *event, void *ctx)
{
    (void)ctx;
    if (!event) return ESP_GMF_ERR_OK;

    if (event->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        esp_gmf_event_state_t st = (esp_gmf_event_state_t)event->sub;
        ESP_LOGD(TAG, "GMF event: state=%s", esp_gmf_event_get_state_str(st));

        switch (st) {
            case ESP_GMF_EVENT_STATE_RUNNING:
                set_state(PS_PLAYING);
                if (s_play_start_us == 0) {
                    s_play_start_us = esp_timer_get_time();
                }
                break;
            case ESP_GMF_EVENT_STATE_PAUSED:
                if (s_user_stopped) set_state(PS_PAUSED);
                break;
            case ESP_GMF_EVENT_STATE_STOPPED:
                if (get_state() != PS_PAUSED) set_state(PS_STOPPED);
                break;
            case ESP_GMF_EVENT_STATE_FINISHED:
                if (get_state() == PS_PAUSED) break;
                xTaskCreatePinnedToCore(delayed_stop_notify, "stop_dly", 3072, NULL, 5, NULL, 1);
                break;
            case ESP_GMF_EVENT_STATE_ERROR:
                set_state(PS_STOPPED);
                break;
            default:
                break;
        }
    } else if (event->type == ESP_GMF_EVT_TYPE_REPORT_INFO
               && event->sub == ESP_GMF_INFO_SOUND) {
        /* 解码器报告音频信息 → 原子重配 I2S（参照 FAKE_POD_NANO） */
        if (event->payload && event->payload_size >= sizeof(esp_gmf_info_sound_t)) {
            esp_gmf_info_sound_t info;
            memcpy(&info, event->payload, sizeof(info));
            int rate = info.sample_rates;
            int bits = info.bits;
            int ch   = info.channels;
            if (rate <= 0) rate = 48000;
            if (bits != 16 && bits != 24 && bits != 32) bits = 16;
            if (ch <= 0 || ch > 2) ch = 2;
            ESP_LOGI(TAG, "Audio info: %d Hz, %d ch, %d bit", rate, ch, bits);
            if (rate != s_last_i2s_rate || bits != s_last_i2s_bits || ch != s_last_i2s_ch) {
                audio_out_set_clk(NULL, rate, ch, bits);
                s_last_i2s_rate = rate;
                s_last_i2s_bits = bits;
                s_last_i2s_ch   = ch;
            }
        }
    }
    return ESP_GMF_ERR_OK;
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
    if (s_alc_el && s_vol_pending >= 0) {
        int vol = s_vol_pending;
        int alc_gain;
        if (vol <= 0) {
            alc_gain = -64;  /* 静音 */
        } else {
            int diff = 100 - vol;
            alc_gain = -(diff * diff * 30) / 10000;
            if (alc_gain < -64) alc_gain = -64;
        }
        esp_gmf_alc_set_gain(s_alc_el, 0, (int8_t)alc_gain);
        esp_gmf_alc_set_gain(s_alc_el, 1, (int8_t)alc_gain);
        ESP_LOGI(TAG, "vol=%d -> alc_gain=%d", vol, alc_gain);
        s_vol_pending = -1;
    }
}

/* ── 自定义 vol_set：映射 0-100 到 ALC 增益
 *    平方曲线：低音量时下降更慢，保留更多有效位
 *    vol=100 → 0dB, vol=50 → -7.5dB, vol=0 → -64dB（静音）
 */
static esp_err_t _my_vol_set(void *handle, int volume)
{
    if (!s_alc_el) return ESP_ERR_INVALID_STATE;
    int alc_gain;
    if (volume <= 0) {
        alc_gain = -64;
    } else {
        int diff = 100 - volume;
        alc_gain = -(diff * diff * 30) / 10000;
        if (alc_gain < -64) alc_gain = -64;
    }
    esp_gmf_alc_set_gain(s_alc_el, 0, (int8_t)alc_gain);
    esp_gmf_alc_set_gain(s_alc_el, 1, (int8_t)alc_gain);
    ESP_LOGD(TAG, "vol=%d -> alc_gain=%d", volume, alc_gain);
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
    s_finish_notify_spawned = false;
    s_near_end_count = 0;
    /* 视频过滤 */
    if (uri && is_video_uri(uri)) {
        ESP_LOGW(TAG, "Rejected video URI: %.128s", uri);
        return;
    }
    if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY);
    /* 新 URI 时保存当前播放位置（直接读 s_state，避免 get_state() 递归锁） */
    if (s_state == PS_PLAYING && s_play_start_us > 0) {
        s_saved_pos_sec = (s_accumulated_ms + (int)((esp_timer_get_time() - s_play_start_us) / 1000LL)) / 1000;
    } else if (s_state == PS_PAUSED) {
        s_saved_pos_sec = s_accumulated_ms / 1000;
    }
    free(s_track_uri);
    s_track_uri = uri ? strdup(uri) : NULL;
    /* 新 URI → 重置时长缓存 + 歌词 */
    s_dur_cache_sec = 0;
    s_cur_title[0] = '\0';
    s_cur_artist[0] = '\0';
    lyrics_clear();
    s_lyrics_ui_clear_pending = true;  /* 通知 UI 任务清除歌词标签 */
    /* 状态转换：设置 URI 后从 NO_MEDIA → STOPPED */
    if (s_state == PS_NO_MEDIA && s_track_uri) {
        s_state = PS_STOPPED;
    }
    s_user_stopped = 0;
    if (s_state_mux) xSemaphoreGive(s_state_mux);
    ESP_LOGI(TAG, "SetURI: %s", s_track_uri ? s_track_uri : "(null)");
}

/* ── 播完处理：支持 next_uri 自动切换 + 防重复触发 ── */
static void delayed_stop_notify(void *arg)
{
    (void)arg;
    if (s_finish_notify_spawned) { vTaskDelete(NULL); return; }
    s_finish_notify_spawned = true;

    /* 冻结最终位置 */
    if (s_play_start_us > 0) {
        s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        s_play_start_us = 0;
    }

    /* 有预载的下一首 → 自动续播（参考 miair-next _check_play_status） */
    if (s_next_uri && s_next_uri[0]) {
        ESP_LOGI(TAG, "Track finished, auto-playing next: %s", s_next_uri);
        s_accumulated_ms = 0;
        s_near_end_count = 0;
        s_finish_notify_spawned = false;
        cb_next();
        vTaskDelete(NULL);
        return;
    }

    /* 否则发 STOPPED，让控制器推下一首 */
    set_state(PS_STOPPED);
    ESP_LOGI(TAG, "Track finished -> STOPPED (dur=%d ms)", s_accumulated_ms);
    s_accumulated_ms = 0;
    s_near_end_count = 0;
    s_finish_notify_spawned = false;
    vTaskDelete(NULL);
}

static void _do_play(const char *uri, int seek_sec)
{
    s_finish_notify_spawned = false;
    s_near_end_count = 0;
    if (!s_pipe || !uri) return;

    /* 重置 I2S 跟踪，确保新歌重新配置 */
    s_last_i2s_rate = 0;
    s_last_i2s_bits = 0;
    s_last_i2s_ch   = 0;

    /* 如果管线已被 stop（如切歌），重置元素状态为 INITIALIZED 才能重新注册 job */
    esp_gmf_pipeline_reset(s_pipe);
    esp_gmf_pipeline_set_in_uri(s_pipe, uri);
    esp_gmf_pipeline_loading_jobs(s_pipe);
    esp_gmf_err_t err = esp_gmf_pipeline_run(s_pipe);
    if (err == ESP_GMF_ERR_OK) {
        set_state(PS_PLAYING);
        s_accumulated_ms = seek_sec * 1000;
        s_play_start_us = esp_timer_get_time();
        s_grace_until = esp_timer_get_time() + 8000000LL;
        s_user_stopped = 0;
        free(s_playing_uri);
        s_playing_uri = strdup(uri);
        if (seek_sec > 0) {
            vTaskDelay(pdMS_TO_TICKS(300));
            /* GMF seek 按字节偏移，粗略估算（128kbps MP3 ≈ 16000 B/s） */
            esp_gmf_pipeline_seek(s_pipe, (uint64_t)seek_sec * 16000);
            ESP_LOGI(TAG, "Resumed from %d s (seek)", seek_sec);
        }
    } else {
        ESP_LOGE(TAG, "esp_gmf_pipeline_run failed: %d", err);
        set_state(PS_STOPPED);
    }
}

static void cb_play(void)
{
    play_state_t cur = get_state();
    ESP_LOGI(TAG, "Play (state=%d, saved_uri=%s, saved_pos=%d)",
             cur, s_saved_uri ? s_saved_uri : "(null)", s_saved_pos_sec);

    if (cur == PS_PLAYING) {
        /* 正在播放中收到 Play：同 URL 忽略，新 URL 停旧播新 */
        if (s_playing_uri && s_track_uri && strcmp(s_playing_uri, s_track_uri) == 0) {
            ESP_LOGI(TAG, "Play while playing, same URL, ignored");
            return;
        }
        ESP_LOGI(TAG, "Play while playing, new URL, switching");
        esp_gmf_pipeline_stop(s_pipe);
        vTaskDelay(pdMS_TO_TICKS(50));
        _do_play(s_track_uri, 0);
        return;
    }

    if (cur == PS_PAUSED && s_pipe) {
        /* 检查暂停期间是否推了新 URL */
        bool uri_changed = s_playing_uri && s_track_uri
                          && strcmp(s_playing_uri, s_track_uri) != 0;
        if (uri_changed) {
            ESP_LOGI(TAG, "URI changed while paused, switching to new track");
            esp_gmf_pipeline_stop(s_pipe);
            vTaskDelay(pdMS_TO_TICKS(50));
            _do_play(s_track_uri, 0);
        } else {
            /* 同 URI 恢复暂停 */
            esp_gmf_pipeline_resume(s_pipe);
            set_state(PS_PLAYING);
            s_play_start_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Resumed from pause (pos=%d ms)", s_accumulated_ms);
        }
        s_grace_until = esp_timer_get_time() + 8000000LL;
        s_user_stopped = 0;
        return;
    }

    /* 新播放 or 断点续播 */
    if (s_track_uri != NULL && s_pipe != NULL) {
        bool same_track = s_saved_uri && s_track_uri
                          && strcmp(s_saved_uri, s_track_uri) == 0
                          && s_saved_pos_sec > 0;
        _do_play(s_track_uri, same_track ? s_saved_pos_sec : 0);
    }
    s_saved_pos_sec = 0;
}

static void cb_pause(void)
{
    play_state_t cur = get_state();
    ESP_LOGI(TAG, "Pause (state=%d)", cur);
    if (s_pipe && cur == PS_PLAYING) {
        /* 冻结累计播放时间 */
        if (s_play_start_us > 0) {
            s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
            s_play_start_us = 0;
        }
        s_saved_pos_sec = s_accumulated_ms / 1000;
        free(s_saved_uri);
        s_saved_uri = s_track_uri ? strdup(s_track_uri) : NULL;
        esp_gmf_err_t err = esp_gmf_pipeline_pause(s_pipe);
        set_state(PS_PAUSED);
        s_user_stopped = 1;
        ESP_LOGI(TAG, "Paused at %d ms (err=%d)", s_accumulated_ms, err);
    }
}

/* ── 播放/暂停切换（旋钮播放按钮） ── */
static void cb_play_toggle(void)
{
    play_state_t cur = get_state();
    if (cur == PS_PLAYING) {
        cb_pause();
    } else {
        cb_play();
    }
}

static void cb_stop(void)
{
    s_finish_notify_spawned = false;
    s_near_end_count = 0;
    ESP_LOGI(TAG, "Stop");
    /* 冻结累计播放时间 */
    if (s_play_start_us > 0) {
        s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        s_play_start_us = 0;
    }
    s_saved_pos_sec = s_accumulated_ms / 1000;
    free(s_saved_uri);
    s_saved_uri = s_track_uri ? strdup(s_track_uri) : NULL;
    if (s_pipe) esp_gmf_pipeline_stop(s_pipe);
    set_state(PS_STOPPED);
    s_user_stopped = 1;
    free(s_next_uri); s_next_uri = NULL;
    free(s_next_metadata); s_next_metadata = NULL;
    ESP_LOGI(TAG, "Stopped at %d ms", s_accumulated_ms);
}

/* ── Seek：拖动进度条 ── */
static void cb_seek(int seconds)
{
    s_finish_notify_spawned = false;
    s_near_end_count = 0;
    ESP_LOGI(TAG, "Seek %d s (dur=%d)", seconds, s_dur_cache_sec);
    if (!s_pipe) return;

    /* GMF seek 需要管线处于 PAUSED/STOPPED/FINISHED 状态 */
    play_state_t cur = get_state();
    bool was_playing = (cur == PS_PLAYING);
    if (was_playing) {
        esp_gmf_pipeline_pause(s_pipe);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 计算字节偏移：从 HTTP IO 获取文件大小 */
    uint64_t byte_pos = 0;
    esp_gmf_info_file_t file_info = {0};
    esp_gmf_io_handle_t in_io = ESP_GMF_PIPELINE_GET_IN_INSTANCE(s_pipe);
    if (in_io) {
        esp_gmf_io_get_info(in_io, &file_info);
    }
    if (file_info.size > 0 && s_dur_cache_sec > 0) {
        byte_pos = ((uint64_t)seconds * file_info.size) / s_dur_cache_sec;
        ESP_LOGI(TAG, "Seek: %d/%d s → byte %llu/%llu",
                 seconds, s_dur_cache_sec, byte_pos, file_info.size);
    } else {
        /* 回退：粗略估算 128kbps MP3 ≈ 16000 B/s */
        byte_pos = (uint64_t)seconds * 16000;
        ESP_LOGW(TAG, "Seek fallback: no file_size/duration, estimate byte=%llu", byte_pos);
    }

    esp_gmf_err_t err = esp_gmf_pipeline_seek(s_pipe, byte_pos);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Seek failed: %d", err);
    }

    /* 更新位置追踪 */
    s_accumulated_ms = seconds * 1000;
    s_play_start_us = esp_timer_get_time();

    if (was_playing) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_gmf_pipeline_resume(s_pipe);
    }
}

static void cb_set_volume(int v)
{
    s_vol = v;
    if (s_mute) {
        s_mute = 0;
    }
    _my_vol_set(NULL, s_mute ? 0 : s_vol);
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
    ESP_LOGI(TAG, "Next (next_uri=%s) state=%d", s_next_uri ? s_next_uri : "(null)", (int)get_state());
    int was_paused = (get_state() == PS_PAUSED);
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    if (s_next_uri && s_next_uri[0]) {
        char *next_meta = s_next_metadata;
        if (s_pipe) {
            esp_gmf_pipeline_stop(s_pipe);
        }
        free(s_track_uri);
        s_track_uri = s_next_uri;
        s_next_uri = NULL;
        s_next_metadata = NULL;
        if (s_pipe) {
            /* _do_play 会探测格式→配置 I2S→reset→loading_jobs→run */
            _do_play(s_track_uri, 0);
            if (was_paused) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_gmf_pipeline_pause(s_pipe);
                set_state(PS_PAUSED);
            }
        }
        custom_dlna_update_uri(s_track_uri, next_meta);
        free(next_meta);
    } else {
        /* 无 next_uri → 模拟自然播完（参考 miair-next next_track()）：
         * 位置设到曲末 + STOPPED，控制端判定自然结束自动推下一首 */
        if (s_pipe) {
            esp_gmf_pipeline_stop(s_pipe);
        }
        if (s_dur_cache_sec > 0) {
            s_accumulated_ms = s_dur_cache_sec * 1000;  /* 位置=曲末 */
        }
        s_play_start_us = 0;
        set_state(PS_STOPPED);
        custom_dlna_update_uri(NULL, NULL);
    }
    custom_dlna_notify_transport_state_async();
}
static void cb_previous(void) {
    s_finish_notify_spawned = false;
    s_near_end_count = 0;
    ESP_LOGI(TAG, "Previous");
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    if (s_pipe && s_track_uri) {
        esp_gmf_pipeline_stop(s_pipe);
        vTaskDelay(pdMS_TO_TICKS(200));
        _do_play(s_track_uri, 0);
    } else {
        if (s_pipe) esp_gmf_pipeline_stop(s_pipe);
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
    char music_id_str[32] = "";
    extract_tag(title, sizeof(title), metadata, "dc:title");
    extract_tag(artist, sizeof(artist), metadata, "upnp:artist");
    extract_tag(album_art, sizeof(album_art), metadata, "upnp:albumArtURI");
    extract_tag(music_id_str, sizeof(music_id_str), metadata, "netease:musicId");
    unsigned long music_id = music_id_str[0] ? strtoul(music_id_str, NULL, 10) : 0;
    ESP_LOGI(TAG, "Extracted: title='%s' artist='%s' musicId=%lu", title, artist, music_id);

    /* 音质切换检测：同 music_id 保留断点续播，不同则清除 */
    if (s_music_id && music_id && music_id != s_music_id) {
        s_saved_pos_sec = 0;
        free(s_saved_uri); s_saved_uri = NULL;
    }
    if (music_id) s_music_id = music_id;

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
        ESP_LOGI(TAG, "Calling lyrics_fetch_async for: %s - %s (songId=%d)", s_cur_title, s_cur_artist, music_id);
        lyrics_fetch_async(s_cur_title, s_cur_artist, music_id);
    }
}

/* ─────────────────────── 专辑封面下载与解码 ─────────────────────── */

/* ── 用原始 socket 下载封面，全部走 PSRAM，避免 esp_http_client 争抢内部 RAM ── */
static int simple_http_get(const char *url, uint8_t **out_data, int *out_len)
{
    /* 解析 URL: http://host[:port]/path（支持 https:// 降级为 http://） */
    const char *p = url;
    bool is_https = (strncmp(p, "https://", 8) == 0);
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (is_https) {
        ESP_LOGW(TAG, "HTTPS cover, downgrading to HTTP");
        p += 8;
    } else {
        ESP_LOGW(TAG, "Only http/https supported"); return -1;
    }

    char host[128] = "";
    int port = is_https ? 443 : 80;
    const char *path = "/";
    /* 提取 host */
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < 127) host[i++] = *p++;
    host[i] = '\0';
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') path = p;

    if (is_https) {
        /* HTTPS 降级为 HTTP 下载（CDN 通常支持） */
        port = 80;
    }

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

/* ─────────────────────── Audio player init (GMF) ─────────────────────── */
static void audio_player_init(void)
{
    /* ── 1. 初始化 I2S + esp_codec_dev（PCM5102A 纯 I2S 输出）── */
    s_codec_dev = audio_out_init();
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "audio_out_init failed!");
        return;
    }

    /* ── 2. 创建 GMF 池，注册所有默认元素 ── */
    esp_gmf_pool_init(&s_pool);
    gmf_loader_setup_io_default(s_pool);
    gmf_loader_setup_audio_codec_default(s_pool);
    gmf_loader_setup_audio_effects_default(s_pool);

    /* ── 3. 创建管线：io_http → aud_dec → aud_alc → io_codec_dev
     *    禁用重采样（aud_rate_cvt 不在管线中），I2S 跟随源采样率
     *    去掉 aud_ch_cvt / aud_bit_cvt（DLNA 源通常是 16-bit 立体声，PCM5102A 直接输出）
     */
    /* 播放前探测格式 → 预配置 I2S 时钟，管线中无需重采样 */
    const char *name[] = {"aud_dec", "aud_alc"};
    esp_gmf_err_t ret = esp_gmf_pool_new_pipeline(s_pool,
        "io_http", name, sizeof(name) / sizeof(char *), "io_codec_dev", &s_pipe);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "esp_gmf_pool_new_pipeline failed: %d", ret);
        return;
    }

    /* ── 4. 注入 codec_dev handle ── */
    esp_gmf_io_codec_dev_set_dev(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(s_pipe), s_codec_dev);

    /* ── 5. 获取解码器和 ALC 元素句柄 ── */
    esp_gmf_pipeline_get_el_by_name(s_pipe, "aud_dec", &s_dec_el);
    esp_gmf_pipeline_get_el_by_name(s_pipe, "aud_alc", &s_alc_el);

    /* ── 6. 创建 GMF 任务并绑定到管线 ── */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.name = "dlna_audio";
    task_cfg.thread.stack = 16 * 1024;
    task_cfg.thread.prio = 8;
    task_cfg.thread.stack_in_ext = true;
    ret = esp_gmf_task_init(&task_cfg, &s_work_task);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "esp_gmf_task_init failed: %d", ret);
        return;
    }
    esp_gmf_pipeline_bind_task(s_pipe, s_work_task);
    esp_gmf_task_set_timeout(s_work_task, 20000);
    /* 注意：不在 init 时 loading_jobs！否则首次 pipeline_run 会跑旧 job（无 URI）导致卡死。
     * 每次 _do_play 中 reset + set_in_uri + loading_jobs + run 即可。 */

    /* ── 7. 设置事件回调 ── */
    esp_gmf_pipeline_set_event(s_pipe, pipeline_event_cb, NULL);

    /* ── 8. 起手音量 ── */
    _my_vol_set(NULL, s_vol);

    ESP_LOGI(TAG, "GMF audio pipeline ready (io_http → aud_dec → aud_alc → ch_cvt → bit_cvt → io_codec_dev)");
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

/* IO13 歌词切换按键（软件消抖 300ms） */
static volatile bool s_lyrics_toggle_pending = false;
static volatile int64_t s_last_lyrics_btn_us = 0;

static void IRAM_ATTR btn_lyrics_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_lyrics_btn_us < 300000) return;  /* 300ms 消抖 */
    s_last_lyrics_btn_us = now;
    s_lyrics_toggle_pending = true;
}

static void lyrics_btn_setup(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << 13),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(13, btn_lyrics_isr, NULL);
    ESP_LOGI(TAG, "Lyrics button ready (IO13)");
}

static void rotary_encoder_setup(void)
{
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return;
    }
    /* 旋钮回调从 LVGL 输入驱动获取（旋转=焦点切换，按下=选中） */
    void (*rot_cb)(void*, int) = NULL;
    void (*btn_cb)(void*)      = NULL;
    lvgl_port_get_encoder_callbacks(&rot_cb, &btn_cb);
    rotary_encoder_config_t cfg = {
        .clk_gpio     = 4,
        .dt_gpio      = 5,
        .sw_gpio      = 6,
        .on_rotate    = rot_cb,
        .on_btn_click = btn_cb,
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

/* ── UTF-8 字符数 → 字节偏移 ── */
static int utf8_byte_offset(const char *s, int char_idx)
{
    int count = 0, offset = 0;
    while (s[offset] && count < char_idx) {
        if ((s[offset] & 0xC0) != 0x80) count++;
        offset++;
    }
    return offset;
}

static int utf8_char_count(const char *s)
{
    int count = 0;
    while (*s) {
        if ((*s & 0xC0) != 0x80) count++;
        s++;
    }
    return count;
}

static void ui_update_task(void *arg)
{
    (void)arg;
    int tick = 0;
    int last_cur_line = -1;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(40));

        lvgl_port_lock();

        /* 检查 IO13 歌词切换请求 */
        if (s_lyrics_toggle_pending) {
            s_lyrics_toggle_pending = false;
            lvgl_port_ui_toggle_lyrics();
        }
        /* 清除歌词标签（新歌/切歌时） */
        if (s_lyrics_ui_clear_pending) {
            s_lyrics_ui_clear_pending = false;
            lvgl_port_ui_lyrics_clear();
            last_cur_line = -1;
        }

        int pos_ms = get_position_ms();
        int pos_sec = pos_ms / 1000;
        ESP_LOGI(TAG, "UI: pos=%d dur=%d state=%d", pos_sec, s_dur_cache_sec, (int)get_state());
        lvgl_port_ui_set_progress(pos_sec, s_dur_cache_sec);
        lvgl_port_ui_set_state((int)get_state());
        /* 音量变化检测：滑动时 SOAP 排队，这里 40ms 检测一次，只应用最新值 */
        if (s_vol != s_last_applied_vol) {
            int hw = s_mute ? 0 : s_vol;
            _my_vol_set(NULL, hw);
            s_last_applied_vol = s_vol;
        }
        lvgl_port_ui_set_volume(s_vol);

        const lyric_data_t *lyr = lyrics_get_data();
        if (lyr && lyr->loaded && lyr->count > 0) {
            int cur = lyrics_get_current_line(pos_ms);
            const char *prev = (cur > 0) ? lyr->lines[cur - 1].text : "";
            const char *curr = lyr->lines[cur].text;
            const char *next = (cur + 1 < lyr->count) ? lyr->lines[cur + 1].text : "";

            /* 行切换时才更新歌词文本（避免复位滚动位置） */
            if (cur != last_cur_line) {
                last_cur_line = cur;
                lvgl_port_ui_lyrics_update(cur, prev, curr, next);
                /* 滚动速率与这句歌词时长相绑定 */
                int line_start = lyr->lines[cur].time_ms;
                int line_end = (cur + 1 < lyr->count)
                    ? lyr->lines[cur + 1].time_ms
                    : s_dur_cache_sec * 1000;
                if (line_end <= line_start) line_end = line_start + 5000;
                lvgl_port_ui_lyrics_scroll_to_end(line_end - line_start);
            }

            /* 每 tick 更新逐字高亮（karaoke） */
            if (cur >= 0 && cur < lyr->count && curr[0]) {
                int total_chars = utf8_char_count(curr);
                int progress;
                /* 优先用 klyric 逐字时间轴，精确到毫秒 */
                int kprog = lyrics_get_karaoke_progress(lyr->lines[cur].time_ms, pos_ms);
                if (kprog >= 0) {
                    progress = kprog;
                } else {
                    /* 回退到字数估算，避免伴奏停顿拖慢高亮 */
                    static bool s_klyric_warned = false;
                    if (!s_klyric_warned) {
                        s_klyric_warned = true;
                        ESP_LOGW(TAG, "klyric: NOT AVAILABLE for line=%d/%d pos=%d (fallback to estimation)",
                            cur, lyr->count, pos_ms);
                    }
                    /* 按行时长估算演唱时长，自适应歌曲节奏 */
                    int line_start = lyr->lines[cur].time_ms;
                    int line_end = (cur + 1 < lyr->count)
                        ? lyr->lines[cur + 1].time_ms
                        : s_dur_cache_sec * 1000;
                    if (line_end <= line_start) line_end = line_start + 5000;
                    int line_dur = line_end - line_start;
                    /* 用行时长的 80% 估算，快歌行短自动快，慢歌行长安逸 */
                    /* 慢歌高亮完最后一字应接近下一句出现，避免干等 */
                    int est_sing = line_dur * 80 / 100;
                    if (est_sing < 800) est_sing = 800;
                    progress = (pos_ms - line_start) * 100 / est_sing;
                }
                if (progress < 0) progress = 0;
                if (progress > 100) progress = 100;
                int char_idx = total_chars * progress / 100;
                int byte_idx = utf8_byte_offset(curr, char_idx);
                lvgl_port_ui_lyrics_karaoke(byte_idx);
            }
        } else {
            last_cur_line = -1;
        }

        /* ── 主动播完检测（参考 miair-next _check_play_status）──
         * 软件位置接近曲末但底层未触发 FINISHED 时的兜底。
         * 连续 30 tick（~1.2s）剩余 <1.5s 时触发。 */
        if (get_state() == PS_PLAYING && s_dur_cache_sec > 0 && !s_finish_notify_spawned) {
            int remain_ms = s_dur_cache_sec * 1000 - get_position_ms();
            if (remain_ms < 1500) {
                if (++s_near_end_count >= 30) {
                    s_near_end_count = 0;
                    ESP_LOGI(TAG, "Software near-end detected (remain=%d), forcing completion", remain_ms);
                    xTaskCreatePinnedToCore(delayed_stop_notify, "stop_dly", 3072, NULL, 5, NULL, 1);
                }
            } else {
                s_near_end_count = 0;
            }
        } else {
            s_near_end_count = 0;
        }

        /* 每 tick 缓慢左移当前行 */
        lvgl_port_ui_lyrics_tick_scroll();

        lvgl_port_unlock();

        if ((++tick % 125) == 0) ESP_LOGI(TAG, "UI tick alive");
    }
}

/* ── WiFi 事件组 ── */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg.capable = false,
            .pmf_cfg.required = false,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 等待连接（最多 15 秒） */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout, continuing...");
    }
}

/* ─────────────────────── Entry point ─────────────────────── */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("ESP_GMF", ESP_LOG_WARN);
    esp_log_level_set("esp_http_client", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("LYRIC", ESP_LOG_DEBUG);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ── 状态互斥锁（必须在其他组件之前创建）── */
    s_state_mux = xSemaphoreCreateMutex();

    /* ── WiFi（直接 ESP-IDF API，无 ADF esp_peripherals）── */
    wifi_init();

    /* ── 主机名 + WiFi 协议/功耗 ── */
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

        
    /* ── DLNA 服务（SSDP + HTTP + SOAP）── */
    start_dlna();

    /* ── 旋钮（UI）── */
    rotary_encoder_setup();
    lyrics_btn_setup();

    /* ── TFT 显示 + LVGL ── */
    tft_init();
    lvgl_port_init(5);

    /* 旋钮焦点框选 → 播放控制 */
    lvgl_port_ui_register_btn_prev_cb(cb_previous);
    lvgl_port_ui_register_btn_play_cb(cb_play_toggle);
    lvgl_port_ui_register_btn_next_cb(cb_next);

    lyrics_init();
    xTaskCreatePinnedToCore(ui_update_task, "ui_update", 4096, NULL, 3, NULL, 0);
}
