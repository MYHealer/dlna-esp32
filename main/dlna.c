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
#include "esp_gmf_io_miplay.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_alc.h"
#include "esp_gmf_event.h"
#include "esp_gmf_info.h"
#include "esp_gmf_audio_element.h"
#include "gmf_loader_setup_defaults.h"

#include "custom_dlna.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "rotary_encoder.h"
#include "tft_display.h"
#include "lvgl_port.h"
#include "lyrics_fetch.h"
#include "miplay.h"
#include "now_playing.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "webp/decode.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mdns.h"
#define lodepng_malloc(s) heap_caps_malloc(s, MALLOC_CAP_SPIRAM)
#define lodepng_free(p)  heap_caps_free(p)
#include "extra/libs/png/lodepng.h"

static const char *TAG = "DLNA_APP";

#define DLNA_DEVICE_UUID "8db0797a-f01a-4949-8f59-51188b18180b"

/**
 * @brief 创建 PSRAM 栈任务（对齐 miplay_create_task / 参考项目 dlna_create_task）。
 *        优先 PSRAM 栈（释放内部 SRAM），失败则回退内部 SRAM 全栈。
 *        注意：回退时不减半栈——调用点的栈大小已按调用链需求确定，
 *        减半会重新引入 stop_dly 栈溢出问题。
 */
static BaseType_t dlna_create_task(TaskFunction_t fn, const char *name,
                                   uint32_t stack_bytes, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core)
{
    TaskHandle_t h = NULL;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        fn, name, stack_bytes, arg, prio, &h, core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        if (handle) *handle = h;
        return pdPASS;
    }
    ESP_LOGW(TAG, "%s: PSRAM stack %u failed, fallback internal", name, (unsigned)stack_bytes);
    h = NULL;
    ret = xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, &h, core);
    if (ret == pdPASS && handle) *handle = h;
    return ret;
}


/* ─────────────────────── 播放状态机 ─────────────────────── */
typedef enum {
    PS_NO_MEDIA      = -1,
    PS_STOPPED       = 0,
    PS_PLAYING       = 1,
    PS_PAUSED        = 2,
    PS_TRANSITIONING = 3,
} play_state_t;

/* GMF 音频管线 */
static esp_gmf_pipeline_handle_t s_pipe       = NULL;
static esp_gmf_task_handle_t     s_work_task  = NULL;
static esp_codec_dev_handle_t    s_codec_dev   = NULL;
static esp_gmf_element_handle_t  s_dec_el     = NULL;
static esp_gmf_element_handle_t  s_alc_el     = NULL;
static esp_gmf_pool_handle_t     s_pool       = NULL;

/* MiPlay GMF 管线：io_miplay → aud_dec → aud_alc → io_codec_dev */
static esp_gmf_pipeline_handle_t s_miplay_pipe    = NULL;
static esp_gmf_task_handle_t     s_miplay_task    = NULL;
static esp_gmf_element_handle_t  s_miplay_alc_el  = NULL;
static RingbufHandle_t           s_ts_ringbuf     = NULL;

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
/* 卡住暂停检测：记录非用户暂停的起始时间 */
static int64_t       s_stuck_paused_since = 0;
/* 歌词 UI 清除请求（新歌切歌时，UI 任务处理） */
static volatile bool s_lyrics_ui_clear_pending = false;
/* MiPlay 活跃标志：旋钮反向控制用 */
static volatile bool s_miplay_active = false;
/* MiPlay 反向播放状态/进度（对齐参考 s_miplay_state/s_miplay_position_*）:
 * UI task 在 MiPlay 激活时用这组值显示图标和进度, 否则旋钮暂停后
 * get_state() 恒为 PLAYING, 图标 40ms 内被刷回播放态。 */
static volatile bool s_miplay_paused = false;
static volatile int  s_miplay_pos_ms = 0;      /* 手机确认的最近位置 */
static int64_t       s_miplay_anchor_us = 0;   /* 播放中位置锚点, 0=冻结 */
static int           s_miplay_accum_ms = 0;    /* 冻结累计 */
/* 位置追踪：纯软件方案（参考 miair-next） */
static int64_t        s_play_start_us     = 0;   /* 播放开始时间（esp_timer us），0=未在播放 */
static int            s_accumulated_ms    = 0;   /* 累计已播放 ms（暂停/停止时冻结） */
static char          *s_playing_uri       = NULL; /* 当前音频管道实际加载的 URI，用于检测暂停时切歌 */
/* 主动播完检测（参考 miair-next _check_play_status）：
 * 软件位置接近曲末但底层未触发 FINISHED 时的兜底，避免控制器等不到 STOPPED 不切歌 */
static int             s_near_end_count    = 0;   /* 连续接近曲末的 tick 数 */
static volatile int    s_media_generation  = 0;   /* 媒体代数：每次新 URI +1，异步任务校验 */
static volatile int   s_album_art_gen = 0;  /* 封面任务代次，切歌时递增取消旧任务 */
/* 封面 worker 任务：队列驱动 + PSRAM 栈（避免内部 SRAM 碎片导致任务创建失败） */
#define ALBUM_ART_QUEUE_LEN 1
static QueueHandle_t  s_album_art_queue = NULL;
static StaticTask_t   s_album_art_tcb;
static StackType_t   *s_album_art_stack = NULL;


/* ── 工具：获取字符串形式的 DLNA 状态 ── */
static const char *state_str(play_state_t s)
{
    switch (s) {
        case PS_PLAYING:       return "PLAYING";
        case PS_PAUSED:        return "PAUSED_PLAYBACK";
        case PS_NO_MEDIA:      return "NO_MEDIA_PRESENT";
        case PS_TRANSITIONING: return "TRANSITIONING";
        default:               return "STOPPED";
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
typedef struct { int generation; } finish_arg_t;
static void delayed_stop_notify(void *arg);
static void cb_play(void);
static void cb_next(void);
static void cb_previous(void);
static void cb_play_toggle(void);

/* ── Now Playing 统一元数据层回调（forward decl）── */
static int get_miplay_display_pos_ms(void);
static void on_miplay_play_state(bool paused);
static void on_miplay_position(int64_t pos_ms);
static void np_on_meta_changed(const np_meta_t *meta);
static void np_on_cover_url(const char *url);
static void np_on_source_changed(np_source_t src);

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
                /* Grace 保护期内忽略降级事件（旧管线的残留事件） */
                if (esp_timer_get_time() < s_grace_until) {
                    ESP_LOGD(TAG, "STOPPED ignored (grace period)");
                    break;
                }
                if (get_state() != PS_PAUSED) set_state(PS_STOPPED);
                break;
            case ESP_GMF_EVENT_STATE_FINISHED:
                if (get_state() == PS_PAUSED) break;
                finish_arg_t *fa = malloc(sizeof(finish_arg_t));
                if (fa) { fa->generation = s_media_generation; }
                dlna_create_task(delayed_stop_notify, "stop_dly", 12 * 1024, fa, 5, NULL, 1);
                break;
            case ESP_GMF_EVENT_STATE_ERROR:
                if (esp_timer_get_time() < s_grace_until) {
                    ESP_LOGD(TAG, "ERROR ignored (grace period)");
                    break;
                }
                /* 小米音箱模式：流中断 → 自动退出 */
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

/* ── 同歌封面复用：记录当前屏幕上封面所属的歌 ──
 * URI 带时间戳/随机参数不可靠，用"歌名+歌手"判同一首。
 * 跨 DLNA/MiPlay 统一生效（用户确认 miplay 和 dlna 的同一首也算同一首）。 */
static char s_cover_song_key[160] = "";  /* "title - artist"，与屏幕封面绑定 */

static bool cover_song_is_same(const char *title, const char *artist)
{
    if (!title || !title[0]) return false;
    char key[160];
    snprintf(key, sizeof(key), "%.100s - %.50s", title, artist ? artist : "");
    return s_cover_song_key[0] && strcmp(key, s_cover_song_key) == 0;
}

static void cover_song_set(const char *title, const char *artist)
{
    snprintf(s_cover_song_key, sizeof(s_cover_song_key), "%.100s - %.50s",
             title ? title : "", artist ? artist : "");
}

static void cover_song_clear(void)
{
    s_cover_song_key[0] = '\0';
}

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
    esp_gmf_alc_set_gain_all(s_alc_el, (int8_t)alc_gain);
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

/* ── 按模式切配置：音乐源检测 + 模式切换 ── */

/* 不区分大小写的子串搜索 */
static bool str_has(const char *s, const char *sub)
{
    if (!s || !sub) return false;
    size_t slen = strlen(s), sublen = strlen(sub);
    if (sublen > slen) return false;
    for (size_t i = 0; i <= slen - sublen; i++) {
        if (strncasecmp(&s[i], sub, sublen) == 0) return true;
    }
    return false;
}

/* 从 URI + User-Agent + 签名检测音乐源，设置模式并切换 UI */
static void detect_and_apply_music_source(const char *uri)
{
    music_source_t src = MUSIC_SRC_NETEASE;  /* 默认网易云配置 */

    /* 音乐指纹检测 — 参考反编译项目 FUN_000c74dc（User-Agent 链式匹配）
     * QQ系列: qqmusic / qqmusic.qq.com / aqqmusic.tc.qq.com / music.tc.qq.com / qq_music / qq_realtime
     * 网易云: cloudmusic / netease / netease_music
     * B站: bilibili / b23.tv
     * 酷狗: kugou / kugou.com
     * 酷我: kuwo / kuwo.cn
     * 喜马拉雅: ximalaya / ximalaya.com */
    const char *ua = custom_dlna_get_user_agent();
    if (str_has(uri, "qqmusic") || str_has(uri, "qq_music") || str_has(uri, "qq_realtime") ||
        str_has(uri, "tc.qq.com") ||
        str_has(ua, "qqmusic") || str_has(ua, "aqqmusic") || str_has(ua, "qq music")) {
        src = MUSIC_SRC_QQ;
        ESP_LOGI(TAG, "Music source: QQ (uri=%s ua=%s)", uri ? uri : "", ua ? ua : "");
    } else if (str_has(ua, "bilibili") || str_has(uri, "bilibili") || str_has(uri, "b23.tv")) {
        src = MUSIC_SRC_BILIBILI;
        ESP_LOGI(TAG, "Music source: Bilibili (ua=%s)", ua ? ua : "");
    } else if (str_has(ua, "kugou") || str_has(uri, "kugou.com") || str_has(uri, "kugou")) {
        src = MUSIC_SRC_KUGOU;
        ESP_LOGI(TAG, "Music source: Kugou (ua=%s)", ua ? ua : "");
    } else if (str_has(ua, "kuwo") || str_has(uri, "kuwo.cn") || str_has(uri, "kuwo")) {
        src = MUSIC_SRC_KUWO;
        ESP_LOGI(TAG, "Music source: Kuwo (ua=%s)", ua ? ua : "");
    } else if (str_has(ua, "ximalaya") || str_has(uri, "ximalaya.com") || str_has(uri, "ximalaya")) {
        src = MUSIC_SRC_XIMALAYA;
        ESP_LOGI(TAG, "Music source: Ximalaya (ua=%s)", ua ? ua : "");
    }

    custom_dlna_set_music_source(src);
}

static void cb_set_uri(const char *uri)
{
    s_media_generation++;
    s_near_end_count = 0;
    /* 视频过滤 */
    if (uri && is_video_uri(uri)) {
        ESP_LOGW(TAG, "Rejected video URI: %.128s", uri);
        return;
    }

    /* 按模式切配置：检测音乐源 + 设置模式 */
    detect_and_apply_music_source(uri);

    if (s_state_mux) xSemaphoreTake(s_state_mux, portMAX_DELAY);
    /* 新 URI 不管是否同一首，进度都归零（新歌从头播，同首歌重播也从0开始） */
    s_saved_pos_sec = 0;
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
    /* 新 URI → 自动播放（不管当前是 STOPPED、PAUSED 还是 TRANSITIONING，新歌都要播） */
    if (s_track_uri && s_pipe) {
        play_state_t st = get_state();
        if (st == PS_STOPPED || st == PS_PAUSED || st == PS_TRANSITIONING) {
            ESP_LOGI(TAG, "Auto-play on SetURI (state=%d)", st);
            cb_play();
        }
    }
}

/* ── 播完处理（参考 miair-next next_track()）──
 * FINISHED 事件 → 冻结位置 → 转交给 cb_next() 统一处理
 * 携带 generation 快照：新 URI 到来后旧任务自动失效 */
static void delayed_stop_notify(void *arg)
{
    finish_arg_t *fa = (finish_arg_t *)arg;
    int gen = fa ? fa->generation : -1;
    free(fa);

    if (gen != s_media_generation) {
        ESP_LOGI(TAG, "Finish notify stale (gen=%d, current=%d)", gen, s_media_generation);
        vTaskDeleteWithCaps(NULL); return;
    }

    /* 冻结最终位置 */
    if (s_play_start_us > 0) {
        s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
        s_play_start_us = 0;
    }

    cb_next();
    vTaskDeleteWithCaps(NULL);
}

static void _do_play(const char *uri, int seek_sec)
{
    s_media_generation++;
    s_near_end_count = 0;
    if (!s_pipe || !uri) return;

    /* &amp; → & 解码：XML 实体只在送入 HTTP 管线时解码，
     * s_uri（GENA 通知用）保持原始 XML 编码不变 */
    char decoded_uri[2048];
    const char *p = uri;
    char *w = decoded_uri;
    while (*p && w < decoded_uri + sizeof(decoded_uri) - 2) {
        if (strncmp(p, "&amp;", 5) == 0) { *w++ = '&'; p += 5; }
        else { *w++ = *p++; }
    }
    *w = '\0';
    uri = decoded_uri;

    /* 重置 I2S 跟踪，确保新歌重新配置 */
    s_last_i2s_rate = 0;
    s_last_i2s_bits = 0;
    s_last_i2s_ch   = 0;

    /* 如果管线已被 stop（如切歌），重置元素状态为 INITIALIZED 才能重新注册 job */
    esp_gmf_pipeline_reset(s_pipe);

    /* ── 按音乐源设置 HTTP User-Agent ──
     * QQ 音乐 CDN 拒绝默认的 "ESP32 HTTP Client/1.0"，返回 403。
     * 参考 miair-next：用浏览器 UA 防止远端拒绝。
     * 网易云和其他源保持默认，一个萝卜一个坑。 */
    esp_gmf_io_handle_t http_io = NULL;
    esp_gmf_pipeline_get_in(s_pipe, &http_io);
    if (http_io) {
        music_source_t src = custom_dlna_get_music_source();
        if (src == MUSIC_SRC_QQ) {
            esp_gmf_io_http_set_user_agent(http_io,
                "Mozilla/5.0 (Linux; Android 12) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120.0.0.0 Mobile Safari/537.36");
        } else {
            esp_gmf_io_http_set_user_agent(http_io, NULL);
        }
    }

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
        ESP_LOGI(TAG, "Play while playing, new URL, switching (saved_pos=%d)", s_saved_pos_sec);
        s_user_stopped = 0;
        s_grace_until = esp_timer_get_time() + 1500000LL;
        esp_gmf_pipeline_stop(s_pipe);
        vTaskDelay(pdMS_TO_TICKS(1000));
        _do_play(s_track_uri, s_saved_pos_sec);
        s_saved_pos_sec = 0;
        return;
    }

    if (cur == PS_PAUSED && s_pipe) {
        /* 检查暂停期间是否推了新 URL */
        bool uri_changed = s_playing_uri && s_track_uri
                          && strcmp(s_playing_uri, s_track_uri) != 0;
        if (uri_changed) {
            ESP_LOGI(TAG, "URI changed while paused, switching to new track (saved_pos=%d)", s_saved_pos_sec);
            s_user_stopped = 0;
            s_grace_until = esp_timer_get_time() + 1500000LL;
            esp_gmf_pipeline_stop(s_pipe);
            vTaskDelay(pdMS_TO_TICKS(1000));
            _do_play(s_track_uri, s_saved_pos_sec);
            s_saved_pos_sec = 0;
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
    ESP_LOGI(TAG, "cb_play_toggle: s_miplay_active=%d", (int)s_miplay_active);
    if (s_miplay_active) {
        /* 对齐参考 cb_pause/cb_play 的 MiPlay 分支: 立即更新本地状态+锚点,
         * 再发 receiver_control。不更新本地状态会导致 UI 图标 40ms 内被刷回。 */
        play_state_t cur = get_state();
        if (cur == PS_PLAYING) {
            /* 冻结 MiPlay 显示位置 */
            s_miplay_accum_ms = get_miplay_display_pos_ms();
            s_miplay_anchor_us = 0;
            s_miplay_paused = true;
            set_state(PS_PAUSED);
            miplay_send_receiver_control("pause", 0);
            ESP_LOGI(TAG, "[MiPlay] knob pause at %d ms", s_miplay_accum_ms);
        } else {
            s_miplay_paused = false;
            s_miplay_anchor_us = esp_timer_get_time();
            set_state(PS_PLAYING);
            miplay_send_receiver_control("resume", 0);
            ESP_LOGI(TAG, "[MiPlay] knob resume");
        }
        return;
    }
    play_state_t cur = get_state();
    if (cur == PS_PLAYING) {
        cb_pause();
    } else {
        cb_play();
    }
}

static void cb_stop(void)
{
    s_media_generation++;
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
    s_user_stopped = 1;
    /* 宽限期：抑制 pipeline_stop 触发的中间 STOPPED 事件
     * 网易云流程: Stop → SetAVTransportURI → Play，可能间隔很短 */
    s_grace_until = esp_timer_get_time() + 2000000LL;
    if (s_pipe) esp_gmf_pipeline_stop(s_pipe);
    set_state(PS_STOPPED);
    free(s_next_uri); s_next_uri = NULL;
    free(s_next_metadata); s_next_metadata = NULL;
    ESP_LOGI(TAG, "Stopped at %d ms", s_accumulated_ms);
}

/* ── Seek：拖动进度条 ── */
static void cb_seek(int seconds)
{
    s_media_generation++;
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

/* ── 视频过滤：检测 URI 是否指向视频文件 ──
 * 注意：网易云 DLNA 投屏用 video/flv MIME + object.item.videoItem，
 * 但实际是纯音频流通过本地代理转发。只检查明确的视频文件扩展名。 */
static bool is_video_uri(const char *uri)
{
    if (!uri) return false;
    /* 只过滤明确的视频容器格式，不包含 .flv（网易云用 FLV 封装音频） */
    const char *exts[] = { ".mp4", ".mov", ".avi", ".mkv", ".wmv",
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
    return false;
}

/* Next — 严格对齐 miair-next next_track() 流程 */
static void cb_next(void) {
    if (s_miplay_active) {
        ESP_LOGI(TAG, "[MiPlay] -> key-next");
        miplay_send_receiver_control("next", 0);
        return;
    }
    ESP_LOGI(TAG, "Next (next_uri=%s) state=%d", s_next_uri ? s_next_uri : "(null)", (int)get_state());
    s_user_stopped = 0;
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    s_near_end_count = 0;
    s_media_generation++;

    if (s_next_uri && s_next_uri[0]) {
        /* ── 有 next_uri：stop → sleep 1.0s → promote → play ── */
        /* 设置宽限期，抑制 pipeline_stop 触发的中间 STOPPED 事件通知控制器
         * （参考项目 notify_state_change() 只在最终状态后调用） */
        s_grace_until = esp_timer_get_time() + 1500000LL;
        if (s_pipe) {
            esp_gmf_pipeline_stop(s_pipe);
        }
        /* 增加延迟到 1.0s，确保完全停止播放并清空硬件缓存（对齐参考项目） */
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 更新播放信息：next → current */
        free(s_track_uri);
        s_track_uri = s_next_uri;
        s_next_uri = NULL;
        char *next_meta = s_next_metadata;
        s_next_metadata = NULL;

        if (s_pipe) {
            /* _do_play 会探测格式→配置 I2S→reset→loading_jobs→run */
            _do_play(s_track_uri, 0);
        }
        custom_dlna_update_uri(s_track_uri, next_meta);
        free(next_meta);
    } else {
        /* ── 无 next_uri：TRANSITIONING 等待手机下发 SetNextAVTransportURI ──
         * QQ 音乐切歌前会先发 SetNextAVTransportURI，但可能延迟。
         * 设 TRANSITIONING 触发 UPnP 事件唤醒手机，最多等 10s。 */
        s_grace_until = esp_timer_get_time() + 1000000LL;
        if (s_pipe) {
            esp_gmf_pipeline_stop(s_pipe);
        }
        vTaskDelay(pdMS_TO_TICKS(500));

        set_state(PS_TRANSITIONING);
        custom_dlna_notify_transport_state_async();
        ESP_LOGI(TAG, "TRANSITIONING: waiting for next URI (max 10s)...");

        int gen = s_media_generation;
        int waited = 0;
        while (waited < 10000) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
            /* 新 URI 到来（SetNextAVTransportURI 已写入 s_next_uri） */
            if (s_next_uri && s_next_uri[0]) {
                ESP_LOGI(TAG, "TRANSITIONING: got next URI after %dms", waited);
                break;
            }
            /* 检测是否有新媒体介入（generation 变了） */
            if (gen != s_media_generation) {
                ESP_LOGI(TAG, "TRANSITIONING: generation changed, abort");
                return;
            }
        }

        if (s_next_uri && s_next_uri[0]) {
            /* 收到下一曲 → 播放 */
            free(s_track_uri);
            s_track_uri = s_next_uri;
            s_next_uri = NULL;
            char *next_meta = s_next_metadata;
            s_next_metadata = NULL;
            if (s_pipe) {
                _do_play(s_track_uri, 0);
            }
            custom_dlna_update_uri(s_track_uri, next_meta);
            free(next_meta);
        } else {
            /* 超时无下一曲 → 模拟自然播完 */
            if (s_dur_cache_sec > 0) {
                s_accumulated_ms = s_dur_cache_sec * 1000;
            }
            s_play_start_us = 0;
            set_state(PS_STOPPED);
            custom_dlna_update_uri(NULL, NULL);
            ESP_LOGI(TAG, "TRANSITIONING: timeout, stopped");
        }
    }
    custom_dlna_notify_transport_state_async();
}
static void cb_previous(void) {
    if (s_miplay_active) {
        ESP_LOGI(TAG, "[MiPlay] -> key-prev");
        miplay_send_receiver_control("prev", 0);
        return;
    }
    s_media_generation++;
    s_near_end_count = 0;
    ESP_LOGI(TAG, "Previous");
    s_user_stopped = 0;
    s_accumulated_ms = 0;
    s_play_start_us = 0;
    s_saved_pos_sec = 0;
    free(s_saved_uri); s_saved_uri = NULL;
    if (s_pipe && s_track_uri) {
        s_grace_until = esp_timer_get_time() + 1000000LL;
        esp_gmf_pipeline_stop(s_pipe);
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

/* ─────────────────────── Now Playing 统一回调 ───────────────────────
 * DLNA/MiPlay 元数据经 now_playing 层上报到这里，统一驱动 UI。
 * Step1 接线阶段：先记录日志，后续逐步接入显示/封面/歌词。 */

static void np_on_meta_changed(const np_meta_t *meta)
{
    if (!meta) return;
    ESP_LOGI(TAG, "NP meta [%s] ep=%lu: %.32s - %.24s (%lums) has_cover=%d",
             meta->source == NP_SRC_DLNA ? "DLNA" :
             meta->source == NP_SRC_MIPLAY ? "MiPlay" : "?",
             (unsigned long)meta->epoch, meta->title, meta->artist,
             (unsigned long)meta->duration_ms, (int)meta->has_cover);

    /* 对齐参考 esp_miplay-main consume_miplay_media_events_locked 的 new_track 分支:
     * 换曲(title 变化)时 position 清零 + 锚点重置。MiPlay 同一 RTSP 会话内切歌
     * 音频管线不重启，on_miplay_media_start 不会再触发，s_accumulated_ms 无人
     * 清零会导致"当前播放时长"继续累计旧曲值。同曲重复上报(title 相同)不重置。 */
    static char s_np_miplay_last_title[128] = "";
    if (meta->source == NP_SRC_MIPLAY && meta->title[0] &&
        strcmp(meta->title, s_np_miplay_last_title) != 0) {
        strlcpy(s_np_miplay_last_title, meta->title, sizeof(s_np_miplay_last_title));
        s_accumulated_ms = 0;
        if (s_play_start_us > 0) s_play_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "NP new track -> position reset to 0");
    }

    /* MiPlay 时长 → 驱动 UI 进度条总长(s_dur_cache_sec)。DLNA 用 DIDL-Lite 独立解析，
     * 此处只填 MiPlay 源，避免覆盖 DLNA 的时长缓存。 */
    if (meta->source == NP_SRC_MIPLAY && meta->duration_ms > 0) {
        int dur_sec = (int)(meta->duration_ms / 1000);
        if (dur_sec > 0) s_dur_cache_sec = dur_sec;
    }

    /* UI 显示（DLNA 原有行为：先清旧封面，再设标题/歌手）
     * 同歌重投：封面保持不刷新（避免割裂感），只有换歌才清 */
    if (meta->title[0] || meta->artist[0]) {
        bool same_song = cover_song_is_same(meta->title, meta->artist);
        lvgl_port_lock();
        if (meta->has_cover && !same_song) {
            lvgl_port_ui_clear_cover();
        }
        if (meta->title[0])  lvgl_port_ui_set_title(meta->title);
        if (meta->artist[0]) lvgl_port_ui_set_artist(meta->artist);
        lvgl_port_unlock();
    }

    /* 歌词：DLNA 与 MiPlay 源均触发（多源回退已支持任意歌曲）
     * MiPlay 源强制 songId=0：s_music_id 是 DLNA metadata 的网易云 ID，
     * 不随 MiPlay 接管清零，直接传会导致拉到上次 DLNA 歌曲的歌词 */
    if (meta->source == NP_SRC_DLNA || meta->source == NP_SRC_MIPLAY) {
        if (meta->title[0]) {
            unsigned long lyric_id = (meta->source == NP_SRC_DLNA) ? s_music_id : 0;
            ESP_LOGI(TAG, "NP lyrics_fetch: %s - %s (songId=%lu)",
                     meta->title, meta->artist, lyric_id);
            lyrics_fetch_async(meta->title, meta->artist, lyric_id);
        }
    }
}

static void np_on_cover_url(const char *url)
{
    if (!url || !url[0]) return;
    /* 同歌重投：封面 URL/base64 重复到达时不重新下载解码，保持当前封面 */
    if (cover_song_is_same(s_cur_title, s_cur_artist)) {
        ESP_LOGI(TAG, "NP cover skipped (same song)");
        return;
    }
    ESP_LOGI(TAG, "NP cover: %.80s...", url);
    /* 触发统一封面 worker 下载/解码（DLNA URL 或 MiPlay base64，worker 自行识别） */
    fetch_album_art_async(url);
}

static void np_on_source_changed(np_source_t src)
{
    ESP_LOGI(TAG, "NP source → %d", (int)src);
    /* 源切换：封面绑定关系失效（DLNA↔MiPlay 换源视为新歌场景，
     * 用户确认同歌跨源也算同一首——但源切换时 title 尚未恢复，
     * 无法判同歌，保守清除绑定让新源重新走一遍封面流程） */
    cover_song_clear();
    /* 清掉上一源残留的封面/歌词 UI（标题等由新源 meta 覆盖） */
    lvgl_port_lock();
    lvgl_port_ui_clear_cover();
    lvgl_port_unlock();
    lyrics_clear();
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

    if (title[0] && (strcmp(title, s_cur_title) != 0 || strcmp(artist, s_cur_artist) != 0)) {
        decode_html_entities(s_cur_title, sizeof(s_cur_title), title);
        decode_html_entities(s_cur_artist, sizeof(s_cur_artist), artist);
        ESP_LOGI(TAG, "Song: %s - %s", s_cur_title, s_cur_artist);

        /* 网易云 CDN 用 ?param=300y300 请求小图；其他源保持 URL 原样 */
        if (album_art[0] && custom_dlna_get_music_source() == MUSIC_SRC_NETEASE) {
            if (!strchr(album_art, '?')) {
                strncat(album_art, "?param=300y300", sizeof(album_art) - strlen(album_art) - 1);
            }
        }

        /* 封面: 对齐 v4.1 直通路径（不进 now_playing 层）。
         * 回归根因: v4.9 起封面经 np_submit→np_on_cover_url 中转,
         * MiPlay 会话残留的 np source/48KB 封面缓冲使 DLNA 下载
         * 256KB 缓冲时败时成("有时有有时没有")。v4.1 证明
         * URL 封面直通 fetch_album_art_async 稳定可靠;
         * np 层仅保留 MiPlay base64 大载荷路径。
         * 同歌重投：URL 带时间戳参数，按歌名判断跳过重新下载。 */
        if (album_art[0] && !cover_song_is_same(s_cur_title, s_cur_artist)) {
            fetch_album_art_async(album_art);
        }

        /* 标题/歌手/时长仍走 now_playing 统一显示（cover_url 置空避免 np 层重复触发下载） */
        np_meta_t m;
        memset(&m, 0, sizeof(m));
        strlcpy(m.title,  s_cur_title,  sizeof(m.title));
        strlcpy(m.artist, s_cur_artist, sizeof(m.artist));
        m.duration_ms = s_dur_cache_sec * 1000;
        /* 歌词仅网易云源拉取（歌词源是网易云 API），随 np 回调内判断源触发 */
        np_submit(NP_SRC_DLNA, &m);
    }
}

/* ─────────────────────── 专辑封面下载与解码 ─────────────────────── */

/* ── HTTPS 封面下载（esp_http_client 支持 TLS）── */
static int https_get_cover(const char *url, uint8_t **out_data, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTPS status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 分配 PSRAM 缓冲区读取响应 */
    int cap = (content_length > 0) ? content_length + 256 : 256 * 1024;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    int total = 0;
    int r;
    while ((r = esp_http_client_read(client, (char *)buf + total, cap - total)) > 0) {
        total += r;
        if (total > cap - 4096) {
            int new_cap = cap + 128 * 1024;
            uint8_t *new_buf = heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!new_buf) break;
            buf = new_buf;
            cap = new_cap;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total < 16) {
        ESP_LOGW(TAG, "HTTPS too small: %d bytes", total);
        heap_caps_free(buf);
        return -1;
    }

    *out_data = buf;
    *out_len = total;
    ESP_LOGI(TAG, "HTTPS cover downloaded: %d bytes", total);
    return 0;
}

/* ── 用原始 socket 下载封面，全部走 PSRAM，避免 esp_http_client 争抢内部 RAM ── */
static int simple_http_get(const char *url, uint8_t **out_data, int *out_len)
{
    /* 解析 URL: http://host[:port]/path */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        /* HTTPS → 委托给 esp_http_client（支持 TLS） */
        return https_get_cover(url, out_data, out_len);
    } else {
        ESP_LOGW(TAG, "Only http/https supported"); return -1;
    }

    char host[128] = "";
    int port = 80;
    const char *path = "/";
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

/* ── 封面源判定/解码辅助（对齐 dlna-esp32-master）──
 * source 可能是 http URL 或 data:...;base64,... 内嵌串。 */

static bool is_http_cover_source(const char *source)
{
    return source && (strncasecmp(source, "http://", 7) == 0 ||
                      strncasecmp(source, "https://", 8) == 0);
}

/* FusionPlay normalizes Xiaomi mArt values to either an URL or a raw/data-URI
 * Base64 image.  Decode the latter locally so an embedded image is not
 * mistaken for an HTTP host name.
 *
 * 小米妙播的 mArt 是 RFC 2045 行折叠 base64（每 76 字符 \n），
 * mbedtls_base64_decode 严格模式拒绝空白 → 必须先剥离。
 * 参考 FusionPlay XiaomiPlaybackSnapshotReducer.kt: filterNot(Char::isWhitespace)。
 */
static int decode_base64_cover(const char *source, uint8_t **out_data, int *out_len)
{
    if (!source || !out_data || !out_len) return -1;
    const char *encoded = source;
    if (strncasecmp(encoded, "data:", 5) == 0) {
        const char *comma = strchr(encoded, ',');
        if (!comma || !strstr(encoded, ";base64")) return -1;
        encoded = comma + 1;
    } else if (strncasecmp(encoded, "http://", 7) == 0 ||
               strncasecmp(encoded, "https://", 8) == 0 ||
               strncasecmp(encoded, "file://", 7) == 0) {
        return -1;
    }
    size_t raw_len = strlen(encoded);
    if (raw_len < 8) {
        ESP_LOGW(TAG, "Cover Base64 too short");
        return -1;
    }
    /* 剥离空白字符（\n \r \t 空格）— RFC 2045 行折叠 base64 必须处理 */
    size_t clean_len = 0;
    char *clean = heap_caps_malloc(raw_len + 1, MALLOC_CAP_SPIRAM);
    if (!clean) return -1;
    for (size_t i = 0; i < raw_len; i++) {
        char c = encoded[i];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            clean[clean_len++] = c;
        }
    }
    clean[clean_len] = '\0';
    if (clean_len != raw_len) {
        ESP_LOGI(TAG, "Cover Base64: stripped %u whitespace chars (%u → %u)",
                 (unsigned)(raw_len - clean_len), (unsigned)raw_len, (unsigned)clean_len);
    }
    if (clean_len < 8) {
        ESP_LOGW(TAG, "Cover Base64 too short after strip");
        heap_caps_free(clean);
        return -1;
    }
    size_t capacity = (clean_len * 3) / 4 + 4;
    uint8_t *decoded = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!decoded) { heap_caps_free(clean); return -1; }
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, capacity, &decoded_len,
                                    (const unsigned char *)clean, clean_len);
    heap_caps_free(clean);  /* 清理临时缓冲 */
    if (ret != 0 || decoded_len < 4) {
        ESP_LOGW(TAG, "Cover Base64 decode failed: ret=%d clean=%u decoded=%u",
                 ret, (unsigned)clean_len, (unsigned)decoded_len);
        heap_caps_free(decoded);
        return -1;
    }
    *out_data = decoded;
    *out_len = (int)decoded_len;
    ESP_LOGI(TAG, "Cover Base64 decoded: %u bytes", (unsigned)decoded_len);
    return 0;
}

static void album_art_task(void *arg)
{
    (void)arg;
    char *url = NULL;
    while (1) {
        if (xQueueReceive(s_album_art_queue, &url, portMAX_DELAY) != pdTRUE) continue;
        if (!url) continue;
        int my_gen = s_album_art_gen;

        /* 等音频管线稳定（1 秒足够首帧解码 + I2S 配置） */
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 切歌后 gen 变化，放弃本次下载 */
        if (s_album_art_gen != my_gen) {
            ESP_LOGI(TAG, "Cover task cancelled (gen mismatch)");
            heap_caps_free(url); continue;
        }

        uint8_t *img_data = NULL;
        int img_len = 0;
        int cover_ret = is_http_cover_source(url) ?
                        simple_http_get(url, &img_data, &img_len) :
                        decode_base64_cover(url, &img_data, &img_len);
        if (cover_ret != 0 || !img_data || img_len < 4) {
            ESP_LOGW(TAG, "Cover load failed: %.160s", url);
            lvgl_port_lock();
            lvgl_port_ui_clear_cover();
            lvgl_port_unlock();
            heap_caps_free(url); continue;
        }

        /* 再次检查：下载期间可能切歌了 */
        if (s_album_art_gen != my_gen) {
            ESP_LOGI(TAG, "Cover task cancelled after download");
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }

    /* 检测图片格式并解码 */
    ESP_LOGI(TAG, "Image data: len=%d, first4=%02X %02X %02X %02X",
             img_len, img_data[0], img_data[1], img_data[2], img_data[3]);
    int out_w = 0, out_h = 0;
    uint16_t *out_buf = NULL;
    bool jpeg_aligned = false;  /* JPEG 用 jpeg_calloc_align 分配 */

    if (img_data[0] == 0xFF && img_data[1] == 0xD8) {
        /* ====== JPEG 解码（esp_new_jpeg，支持 progressive） ====== */
        /* scale 限制解码输出 ≤ 384×384（= 48×8），避免大图全尺寸解码耗尽 PSRAM。
         * esp_new_jpeg 要求 scale 值为 8 的倍数，最大缩放比 1/8。 */
        jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
        dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;  /* 小端序，CPU 直接读 */
        dec_cfg.scale.width  = 384;
        dec_cfg.scale.height = 384;
        jpeg_dec_handle_t jpeg_dec = NULL;
        jpeg_error_t jerr = jpeg_dec_open(&dec_cfg, &jpeg_dec);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG open failed: %d", jerr);
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }

        jpeg_dec_io_t io = { .inbuf = img_data, .inbuf_len = img_len };
        jpeg_dec_header_info_t info;
        jerr = jpeg_dec_parse_header(jpeg_dec, &io, &info);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG header parse failed: %d", jerr);
            jpeg_dec_close(jpeg_dec);
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }
        out_w = info.width;
        out_h = info.height;

        int outbuf_len = 0;
        jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
        uint8_t *raw_buf = (uint8_t *)jpeg_calloc_align(outbuf_len, 16);
        if (!raw_buf) {
            ESP_LOGW(TAG, "JPEG outbuf alloc failed (%d)", outbuf_len);
            jpeg_dec_close(jpeg_dec);
            heap_caps_free(img_data); heap_caps_free(url); continue;
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
            heap_caps_free(img_data); heap_caps_free(url); continue;
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
            heap_caps_free(img_data); heap_caps_free(url); continue;
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
        /* 尺寸预检：lodepng_decode24 分配 w*h*3 字节，大图会耗尽 PSRAM */
        {
            unsigned insp_err = lodepng_inspect(&png_w, &png_h, NULL, img_data, img_len);
            if (insp_err || png_w <= 1 || png_h <= 1) {
                ESP_LOGW(TAG, "PNG header invalid: %u", insp_err);
                heap_caps_free(img_data); heap_caps_free(url); continue;
            }
        }
        if (png_w > 512 || png_h > 512) {
            ESP_LOGW(TAG, "PNG too large: %ux%u, skip (max 512)", png_w, png_h);
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }
        uint8_t *png_rgb = NULL;
        /* 用 RGB（3 字节/像素）而非 RGBA，省 25% 内存 */
        unsigned err = lodepng_decode24(&png_rgb, &png_w, &png_h, img_data, img_len);
        if (err) {
            ESP_LOGW(TAG, "PNG decode failed: %u (%s), w=%u h=%u", err, lodepng_error_text(err), png_w, png_h);
            heap_caps_free(img_data);
            heap_caps_free(url);
            continue;
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
            heap_caps_free(url);
            continue;
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

    } else if (img_data[0] == 'R' && img_data[1] == 'I' && img_data[2] == 'F' && img_data[3] == 'F' &&
               img_len >= 12 &&
               img_data[8] == 'W' && img_data[9] == 'E' && img_data[10] == 'B' && img_data[11] == 'P') {
        /* ====== WebP 解码（libwebp WebPDecodeRGB → RGB888，对齐 dlna-esp32-master） ====== */
        /* 尺寸预检：WebPDecodeRGB 分配 w*h*3 字节 PSRAM，大图会耗尽内存 */
        int webp_w = 0, webp_h = 0;
        if (!WebPGetInfo(img_data, (size_t)img_len, &webp_w, &webp_h) ||
            webp_w <= 1 || webp_h <= 1) {
            ESP_LOGW(TAG, "WebP header invalid");
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }
        if (webp_w > 512 || webp_h > 512) {
            ESP_LOGW(TAG, "WebP too large: %dx%d, skip (max 512)", webp_w, webp_h);
            heap_caps_free(img_data); heap_caps_free(url); continue;
        }
        uint8_t *webp_rgb = WebPDecodeRGB(img_data, (size_t)img_len, &webp_w, &webp_h);
        if (!webp_rgb) {
            ESP_LOGW(TAG, "WebP decode failed: %dx%d bytes=%d", webp_w, webp_h, img_len);
            heap_caps_free(img_data);
            heap_caps_free(url);
            continue;
        }
        /* 下载缓冲区已无用，立即释放 */
        heap_caps_free(img_data);
        img_data = NULL;

        /* 中心裁剪 + 缩放到 48x48（RGB888 → BGR565） */
        int min_dim = webp_w < webp_h ? webp_w : webp_h;
        int crop_sz = min_dim;
        int crop_x = (webp_w - crop_sz) / 2;
        int crop_y = (webp_h - crop_sz) / 2;
        #define MAX_COVER 48
        int sw = MAX_COVER, sh = MAX_COVER;
        out_buf = (uint16_t *)heap_caps_malloc((size_t)sw * sh * 2, MALLOC_CAP_SPIRAM);
        if (!out_buf) {
            ESP_LOGW(TAG, "WebP out_buf alloc failed");
            WebPFree(webp_rgb);
            heap_caps_free(url);
            continue;
        }
        for (int y = 0; y < sh; y++) {
            float fy = crop_y + (float)y / sh * crop_sz;
            for (int x = 0; x < sw; x++) {
                float fx = crop_x + (float)x / sw * crop_sz;
                int ix = (int)fx, iy = (int)fy;
                if (ix >= webp_w - 1) ix = webp_w - 2;
                if (iy >= webp_h - 1) iy = webp_h - 2;
                float dx = fx - ix, dy = fy - iy;
                int base = iy * webp_w + ix;
                int r = (int)((webp_rgb[base*3]*(1-dx) + webp_rgb[(base+1)*3]*dx)*(1-dy) +
                              (webp_rgb[(base+webp_w)*3]*(1-dx) + webp_rgb[(base+webp_w+1)*3]*dx)*dy);
                int g = (int)((webp_rgb[base*3+1]*(1-dx) + webp_rgb[(base+1)*3+1]*dx)*(1-dy) +
                              (webp_rgb[(base+webp_w)*3+1]*(1-dx) + webp_rgb[(base+webp_w+1)*3+1]*dx)*dy);
                int b = (int)((webp_rgb[base*3+2]*(1-dx) + webp_rgb[(base+1)*3+2]*dx)*(1-dy) +
                              (webp_rgb[(base+webp_w)*3+2]*(1-dx) + webp_rgb[(base+webp_w+1)*3+2]*dx)*dy);
                uint16_t c = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
                out_buf[y * sw + x] = (c >> 8) | (c << 8);
            }
        }
        WebPFree(webp_rgb);
        out_w = sw; out_h = sh;
        ESP_LOGI(TAG, "WebP decoded: %ux%u -> %dx%d (crop=%d)", webp_w, webp_h, out_w, out_h, crop_sz);

    } else {
        ESP_LOGW(TAG, "Unknown format: %02X %02X %02X %02X",
                 img_data[0], img_data[1], img_data[2], img_data[3]);
        heap_caps_free(img_data);
        heap_caps_free(url);
        continue;
    }

    /* 显示封面（数据通过 memcpy 到内部 RAM 静态缓冲区，无缓存问题） */
    lvgl_port_lock();
    lvgl_port_ui_set_cover(out_buf, out_w, out_h);
    lvgl_port_unlock();

    /* 封面成功上屏后登记 song_key：此后同歌重投（DLNA/MiPlay）均复用此封面 */
    cover_song_set(s_cur_title, s_cur_artist);
    ESP_LOGI(TAG, "Cover bound to song: %.60s", s_cover_song_key);

    /* 清理 */
    if (jpeg_aligned) jpeg_free_align(out_buf);
    else heap_caps_free(out_buf);
    heap_caps_free(img_data);
    heap_caps_free(url);
    }
}

static void fetch_album_art_async(const char *url)
{
    /* 统一 PSRAM 拷贝（配 album_art_task 的 heap_caps_free）：
     * - DLNA URL 几百字节，PSRAM 无压力
     * - MiPlay base64 十几 KB（实测 15699B），必须 PSRAM
     * 真正的内存回归不在 strdup/PSRAM 之别，而在 np 层 48KB 常驻缓冲
     * 残留 + 256KB 下载缓冲竞争——DLNA 封面已改回 v4.1 直通路径。 */
    if (!url || !url[0] || !s_album_art_queue) return;
    s_album_art_gen++;  /* 递增代次，worker 检测到 gen 变化会放弃旧请求 */
    size_t src_len = strlen(url);
    char *source = (char *)heap_caps_malloc(src_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!source) {
        ESP_LOGW(TAG, "cover source alloc %uB in SPIRAM failed", (unsigned)(src_len + 1));
        return;
    }
    memcpy(source, url, src_len + 1);
    /* 覆盖式发送：队列长度 1，新请求直接替换旧请求 */
    xQueueOverwrite(s_album_art_queue, &source);
}

/* 创建封面 worker 任务：PSRAM 栈，避免内部 SRAM 碎片导致创建失败 */
static void album_art_worker_init(void)
{
    s_album_art_queue = xQueueCreate(ALBUM_ART_QUEUE_LEN, sizeof(char *));
    s_album_art_stack = heap_caps_malloc(24 * 1024, MALLOC_CAP_SPIRAM);
    if (!s_album_art_queue || !s_album_art_stack) {
        ESP_LOGW(TAG, "album_art worker init failed");
        return;
    }
    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        album_art_task, "album_art", 24 * 1024 / sizeof(StackType_t),
        NULL, 1, s_album_art_stack, &s_album_art_tcb, 1);
    if (!handle) {
        ESP_LOGW(TAG, "album_art worker create failed");
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
    task_cfg.thread.stack = 24 * 1024;
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

    /* ── 7. 设置管线事件回调 ── */
    esp_gmf_pipeline_set_event(s_pipe, pipeline_event_cb, NULL);

    /* ── 8. 起手音量 ── */
    _my_vol_set(NULL, s_vol);

    ESP_LOGI(TAG, "GMF audio pipeline ready (io_http → aud_dec → aud_alc → ch_cvt → bit_cvt → io_codec_dev)");
}

/* ─────────────────────── MiPlay GMF 管线 ─────────────────────── */
static esp_gmf_err_t miplay_pipeline_event_cb(esp_gmf_event_pkt_t *event, void *ctx)
{
    (void)ctx;
    if (!event) return ESP_GMF_ERR_OK;

    if (event->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        esp_gmf_event_state_t st = (esp_gmf_event_state_t)event->sub;
        switch (st) {
            case ESP_GMF_EVENT_STATE_RUNNING:
                ESP_LOGI(TAG, "[MiPlay] Pipeline running");
                /* 驱动 DLNA 播放状态机：否则 UI 进度 get_state() 保持旧状态
                 * (STOPPED) → cb_get_position_ms 恒 0，进度条永远 0:00。
                 * s_play_start_us 仅在 0 时置位(对齐 DLNA pipeline_event_cb)。 */
                set_state(PS_PLAYING);
                if (s_play_start_us == 0) s_play_start_us = esp_timer_get_time();
                break;
            case ESP_GMF_EVENT_STATE_FINISHED:
                ESP_LOGI(TAG, "[MiPlay] Pipeline finished");
                break;
            case ESP_GMF_EVENT_STATE_STOPPED:
                ESP_LOGI(TAG, "[MiPlay] Pipeline stopped");
                if (get_state() == PS_PLAYING) set_state(PS_STOPPED);
                break;
            case ESP_GMF_EVENT_STATE_ERROR:
                ESP_LOGE(TAG, "[MiPlay] Pipeline error");
                set_state(PS_STOPPED);
                break;
            default:
                break;
        }
    } else if (event->type == ESP_GMF_EVT_TYPE_REPORT_INFO
               && event->sub == ESP_GMF_INFO_SOUND) {
        if (event->payload && event->payload_size >= sizeof(esp_gmf_info_sound_t)) {
            esp_gmf_info_sound_t info;
            memcpy(&info, event->payload, sizeof(info));
            int rate = info.sample_rates;
            int bits = info.bits;
            int ch   = info.channels;
            if (rate <= 0) rate = 48000;
            if (bits != 16 && bits != 24 && bits != 32) bits = 16;
            if (ch <= 0 || ch > 2) ch = 2;
            ESP_LOGI(TAG, "[MiPlay] Audio info: %d Hz, %d ch, %d bit", rate, ch, bits);
            audio_out_set_clk(NULL, rate, ch, bits);
        }
    }
    return ESP_GMF_ERR_OK;
}

/* Forward declarations for MiPlay pipeline callbacks */
static esp_gmf_err_t miplay_pipeline_event_cb(esp_gmf_event_pkt_t *event, void *ctx);
static void on_miplay_vol_changed(uint32_t vol_percent);
static void on_miplay_media_start(bool start);

static void miplay_pipeline_init(void)
{
    if (!s_codec_dev) {
        s_codec_dev = audio_out_init();
    }

    /* 先创建 ring buffer，后面注入管线要用 */
    s_ts_ringbuf = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_ts_ringbuf) {
        ESP_LOGE(TAG, "TS ring buffer create failed");
        return;
    }
    miplay_set_ts_ringbuf(s_ts_ringbuf);

    /* 注册 io_miplay 到共享 pool */
    miplay_io_cfg_t miplay_cfg = ESP_GMF_IO_MIPLAY_CFG_DEFAULT();
    miplay_cfg.dir = ESP_GMF_IO_DIR_READER;
    esp_gmf_io_handle_t miplay_io = NULL;
    esp_gmf_err_t ret = esp_gmf_io_miplay_init(&miplay_cfg, &miplay_io);
    if (ret != ESP_GMF_ERR_OK || !miplay_io) {
        ESP_LOGE(TAG, "esp_gmf_io_miplay_init failed: %d", ret);
        return;
    }
    esp_gmf_pool_register_io(s_pool, miplay_io, "io_miplay");

    /* 管线：io_miplay → aud_dec → aud_alc → io_codec_dev */
    const char *name[] = {"aud_dec", "aud_alc"};
    ret = esp_gmf_pool_new_pipeline(s_pool,
        "io_miplay", name, sizeof(name) / sizeof(char *), "io_codec_dev", &s_miplay_pipe);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "MiPlay pipeline create failed: %d", ret);
        return;
    }

    esp_gmf_io_codec_dev_set_dev(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(s_miplay_pipe), s_codec_dev);

    /* 对管线内的 io_miplay 副本注入 ringbuf（pool 复制时 ringbuf=NULL） */
    esp_gmf_io_miplay_set_ringbuf(ESP_GMF_PIPELINE_GET_IN_INSTANCE(s_miplay_pipe), s_ts_ringbuf);

    esp_gmf_pipeline_get_el_by_name(s_miplay_pipe, "aud_alc", &s_miplay_alc_el);

    /* 创建 GMF 任务 */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.name = "miplay_audio";
    task_cfg.thread.stack = 24 * 1024;
    task_cfg.thread.prio = 8;
    task_cfg.thread.stack_in_ext = true;
    ret = esp_gmf_task_init(&task_cfg, &s_miplay_task);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "MiPlay task init failed: %d", ret);
        return;
    }
    esp_gmf_pipeline_bind_task(s_miplay_pipe, s_miplay_task);
    esp_gmf_task_set_timeout(s_miplay_task, 20000);

    esp_gmf_pipeline_set_event(s_miplay_pipe, miplay_pipeline_event_cb, NULL);
    miplay_set_vol_changed_cb(on_miplay_vol_changed);
    miplay_set_media_cb(on_miplay_media_start);
    miplay_set_play_state_cb(on_miplay_play_state);
    miplay_set_position_cb(on_miplay_position);

    ESP_LOGI(TAG, "MiPlay GMF pipeline ready (io_miplay → aud_dec → aud_alc → io_codec_dev)");
}

static void miplay_pipeline_start(void)
{
    if (!s_miplay_pipe) { ESP_LOGW(TAG, "[MiPlay] pipeline not initialized!"); return; }
    ESP_LOGW(TAG, "[MiPlay] >>> Starting GMF pipeline <<<");
    /* 清空 ring buffer 中残留数据 */
    size_t dummy;
    void *item;
    while ((item = xRingbufferReceive(s_ts_ringbuf, &dummy, 0)) != NULL) {
        vRingbufferReturnItem(s_ts_ringbuf, item);
    }
    /* 重连后元素状态残留 STOPPED（上一次 pipeline_stop 关闭了 aud_dec/aud_alc），
     * loading_jobs 要求元素为 INITIALIZED 才注册 job，否则静默跳过 →
     * pipeline_run 等不到 job 20s 超时(-3) → 无声。
     * 与 DLNA _do_play 的 esp_gmf_pipeline_reset 同理（esp_gmf_pipeline.c:55）。 */
    esp_gmf_pipeline_reset(s_miplay_pipe);
    esp_gmf_err_t r1 = esp_gmf_pipeline_loading_jobs(s_miplay_pipe);
    ESP_LOGI(TAG, "[MiPlay] loading_jobs returned %d", r1);
    esp_gmf_err_t r2 = esp_gmf_pipeline_run(s_miplay_pipe);
    ESP_LOGI(TAG, "[MiPlay] pipeline_run returned %d", r2);
}

static void miplay_pipeline_stop(void)
{
    if (!s_miplay_pipe) return;
    ESP_LOGI(TAG, "[MiPlay] Stopping GMF pipeline");
    esp_gmf_pipeline_stop(s_miplay_pipe);
    /* 排空 ring buffer */
    size_t dummy;
    void *item;
    while ((item = xRingbufferReceive(s_ts_ringbuf, &dummy, 0)) != NULL) {
        vRingbufferReturnItem(s_ts_ringbuf, item);
    }
}

/* MiPlay 媒体流开始/停止 → 启动/停止 GMF 管线 */
static void on_miplay_media_start(bool start)
{
    if (start) {
        ESP_LOGW(TAG, "=== MiPlay media started → starting GMF pipeline ===");
        s_miplay_active = true;    /* MiPlay 开播即进入反控模式 */
        /* 新歌从 0 累计：清旧曲残留位置，避免切歌后进度续算旧值 */
        s_accumulated_ms = 0;
        s_play_start_us = 0;
        miplay_pipeline_start();
    } else {
        ESP_LOGW(TAG, "=== MiPlay media stopped → stopping GMF pipeline ===");
        s_miplay_active = false;
        /* 切歌/停止：冻结累计并复位起点。RUNNING 事件会再置 s_play_start_us。 */
        if (s_play_start_us > 0) {
            s_accumulated_ms += (int)((esp_timer_get_time() - s_play_start_us) / 1000LL);
            s_play_start_us = 0;
        }
        miplay_pipeline_stop();
    }
}

/* MiPlay 手机端音量变化 → 同步到 GMF ALC */
static void on_miplay_vol_changed(uint32_t vol_percent)
{
    if (!s_miplay_alc_el) return;
    int alc_gain;
    if (vol_percent <= 0) {
        alc_gain = -64;
    } else {
        int diff = 100 - (int)vol_percent;
        alc_gain = -(diff * diff * 30) / 10000;
        if (alc_gain < -64) alc_gain = -64;
    }
    esp_gmf_alc_set_gain_all(s_miplay_alc_el, (int8_t)alc_gain);
    ESP_LOGI(TAG, "[MiPlay] Vol %lu%% → ALC gain %d", (unsigned long)vol_percent, alc_gain);
}

/* ── MiPlay 反向播放状态（手机 0x0004 pause / 0x0006 resume）──
 * 对齐参考 consume_miplay_media_events_locked 的 PLAYER_STATE 分支:
 * 冻结/重置位置锚点并记录状态, UI task 据此显示图标。 */
static void on_miplay_play_state(bool paused)
{
    s_miplay_paused = paused;
    if (paused) {
        if (s_miplay_anchor_us > 0) {
            s_miplay_accum_ms = s_miplay_pos_ms;  /* 冻结在手机确认位置 */
            s_miplay_anchor_us = 0;
        }
    } else {
        s_miplay_anchor_us = esp_timer_get_time();
    }
    ESP_LOGI(TAG, "[MiPlay] phone %s (pos=%dms)", paused ? "PAUSED" : "PLAYING",
             s_miplay_pos_ms);
}

/* ── MiPlay 位置同步（手机 SetPosition 0x0056, 大端毫秒）──
 * 对齐参考 dlna.c:671-675: 位置写入 + 播放中重置锚点。 */
static void on_miplay_position(int64_t pos_ms)
{
    if (pos_ms < 0) pos_ms = 0;
    s_miplay_pos_ms = (int)pos_ms;
    s_miplay_accum_ms = (int)pos_ms;
    if (!s_miplay_paused && s_miplay_active)
        s_miplay_anchor_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[MiPlay] pos sync %lldms (paused=%d)", (long long)pos_ms,
             (int)s_miplay_paused);
}

/* MiPlay 激活时的显示位置（对齐参考 get_miplay_position_ms） */
static int get_miplay_display_pos_ms(void)
{
    int64_t pos = s_miplay_accum_ms;
    if (s_miplay_active && !s_miplay_paused && s_miplay_anchor_us > 0)
        pos += (esp_timer_get_time() - s_miplay_anchor_us) / 1000LL;
    return (int)pos;
}

/* ─────────────────────── Rotary encoder ─────────────────────── */
static void _enc_on_btn(void *arg)
{
    (void)arg;
    play_state_t cur = get_state();
    ESP_LOGI(TAG, "[Knob] press state=%d", cur);
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
    ESP_LOGI(TAG, "[Knob] rotate dir=%d vol %d->%d", direction, s_vol, new_vol);
    cb_set_volume(new_vol);
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
    ESP_LOGI(TAG, "Custom DLNA started (host=ESP32-MIPLAY)");
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

/* ── 末句时长估算（移植 MeloX：min(max(可唱字数*0.32s, 2s), 8s)）──
 * 整首歌末尾那句没有下句时间戳，若用总时长兜底会让滚动/高亮长时间停滞。
 * 按这句实际可唱字数估算其显示时长，长句给够、短句不白等。 */
static int estimate_last_line_duration_ms(const char *text)
{
    int chars = utf8_char_count(text);
    if (chars < 1) chars = 1;
    int est = chars * 320;            /* 0.32 s/字 */
    if (est < 2000) return 2000;      /* 下限 2s */
    if (est > 8000) return 8000;      /* 上限 8s */
    return est;
}

static void ui_update_task(void *arg)
{
    (void)arg;
    int tick = 0;
    int last_cur_line = -1;
    while (1) {
        play_state_t ui_state = get_state();
        /* 空闲态降频到 2Hz，播放态保持 25Hz */
        vTaskDelay(pdMS_TO_TICKS((ui_state == PS_PLAYING || ui_state == PS_PAUSED) ? 40 : 500));

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

        int pos_ms, pos_sec;
        /* 对齐参考 UI task: MiPlay 激活时状态/进度走 MiPlay 源,
         * 否则旋钮暂停后图标会被 get_state() 恒 PLAYING 刷回。 */
        bool miplay_ui = s_miplay_active;
        play_state_t display_state = miplay_ui
            ? (s_miplay_paused ? PS_PAUSED : PS_PLAYING)
            : get_state();
        if (miplay_ui) {
            pos_ms = get_miplay_display_pos_ms();
            pos_sec = pos_ms / 1000;
            int miplay_dur = (s_dur_cache_sec > 0) ? s_dur_cache_sec : 0;
            ESP_LOGI(TAG, "UI: pos=%d dur=%d state=%d [miplay]", pos_sec, miplay_dur,
                     (int)display_state);
            lvgl_port_ui_set_progress(pos_sec, miplay_dur);
            lvgl_port_ui_set_state((int)display_state);
        } else {
            pos_ms = get_position_ms();
            pos_sec = pos_ms / 1000;
            ESP_LOGI(TAG, "UI: pos=%d dur=%d state=%d", pos_sec, s_dur_cache_sec, (int)get_state());
            lvgl_port_ui_set_progress(pos_sec, s_dur_cache_sec);
            lvgl_port_ui_set_state((int)get_state());
        }
        /* 音量变化检测：滑动时 SOAP 排队，这里 40ms 检测一次，只应用最新值 */
        if (s_vol != s_last_applied_vol) {
            int hw = s_mute ? 0 : s_vol;
            _my_vol_set(NULL, hw);
            s_last_applied_vol = s_vol;
        }
        lvgl_port_ui_set_volume(s_vol);

        /* 歌词界面可见时才更新（不可见时跳过，避免 label 在未加载屏幕上取错宽度） */
        const lyric_data_t *lyr = lyrics_get_data();
        if (lyr && lyr->loaded && lyr->count > 0 && lvgl_port_ui_lyrics_is_visible()) {
            int cur = lyrics_get_current_line(pos_ms);
            const char *prev = (cur > 0) ? lyr->lines[cur - 1].text : "";
            const char *curr = lyr->lines[cur].text;
            const char *next = (cur + 1 < lyr->count) ? lyr->lines[cur + 1].text : "";

            /* 行切换时才更新歌词文本（避免复位滚动位置） */
            if (cur != last_cur_line) {
                last_cur_line = cur;
                lvgl_port_ui_lyrics_update(cur, prev, curr, next);
                /* 滚动速率与这句歌词时长相绑定（末句按字数估算，避免滚动停滞） */
                int line_start = lyr->lines[cur].time_ms;
                int line_end;
                if (cur + 1 < lyr->count) {
                    line_end = lyr->lines[cur + 1].time_ms;
                } else {
                    line_end = line_start + estimate_last_line_duration_ms(curr);
                }
                if (line_end <= line_start) line_end = line_start + 5000;
                lvgl_port_ui_lyrics_scroll_to_end(line_end - line_start);
            }

            /* 每 tick 更新逐字高亮（karaoke） */
            if (cur >= 0 && cur < lyr->count && curr[0]) {
                /* 优先用精确字节偏移（klyric/pseudo-klyric 逐字时间戳） */
                int byte_idx = lyrics_get_karaoke_byte_idx(cur, curr, pos_ms);
                if (byte_idx < 0) {
                    /* 回退到百分比估算 */
                    int total_chars = utf8_char_count(curr);
                    int line_start = lyr->lines[cur].time_ms;
                    int line_end;
                    if (cur + 1 < lyr->count) {
                        line_end = lyr->lines[cur + 1].time_ms;
                    } else {
                        line_end = line_start + estimate_last_line_duration_ms(curr);
                    }
                    if (line_end <= line_start) line_end = line_start + 5000;
                    int line_dur = line_end - line_start;
                    int est_sing = line_dur * 80 / 100;
                    if (est_sing < 800) est_sing = 800;
                    int progress = (pos_ms - line_start) * 100 / est_sing;
                    if (progress < 0) progress = 0;
                    if (progress > 100) progress = 100;
                    int char_idx = total_chars * progress / 100;
                    byte_idx = utf8_byte_offset(curr, char_idx);
                }
                lvgl_port_ui_lyrics_karaoke(byte_idx);
            }
        } else if (!lyr || !lyr->loaded || lyr->count <= 0) {
            last_cur_line = -1;
        }

        /* ── 主动播完检测（参考 miair-next _check_play_status）──
         * 软件位置接近曲末但底层未触发 FINISHED 时的兜底。
         * 对齐参考项目：剩余 <1.0s，连续 2 次检测（≈2s 窗口，25Hz 下 50 tick）。 */
        if (get_state() == PS_PLAYING && s_dur_cache_sec > 0) {
            int remain_ms = s_dur_cache_sec * 1000 - get_position_ms();
            if (remain_ms <= 0) {
                /* remain_ms <= 0 说明已到曲末或时长被下一首覆盖，跳过 */
                s_near_end_count = 0;
            } else if (remain_ms < 1000) {
                if (++s_near_end_count >= 50) {
                    s_near_end_count = 0;
                    ESP_LOGI(TAG, "Software near-end detected (remain=%d), forcing completion", remain_ms);
                    finish_arg_t *fa = malloc(sizeof(finish_arg_t));
                    if (fa) { fa->generation = s_media_generation; }
                    dlna_create_task(delayed_stop_notify, "stop_dly", 12 * 1024, fa, 5, NULL, 1);
                }
            } else {
                s_near_end_count = 0;
            }
        } else {
            s_near_end_count = 0;
        }

        /* ── 卡住暂停检测（参考 miair-next stuck-paused）──
         * 非用户暂停超过 30 秒 → 自动复位到 STOPPED */
        if (get_state() == PS_PAUSED && !s_user_stopped) {
            if (s_stuck_paused_since == 0) {
                s_stuck_paused_since = esp_timer_get_time();
            } else if ((esp_timer_get_time() - s_stuck_paused_since) > 30000000LL) {
                ESP_LOGW(TAG, "Stuck paused for 30s, resetting to STOPPED");
                if (s_pipe) esp_gmf_pipeline_stop(s_pipe);
                set_state(PS_STOPPED);
                s_stuck_paused_since = 0;
            }
        } else {
            s_stuck_paused_since = 0;
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
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* WiFi 断连时暂停播放（避免管线报错），重连后自动恢复 */
        if (s_pipe && get_state() == PS_PLAYING) {
            ESP_LOGW(TAG, "WiFi lost, pausing playback");
            esp_gmf_pipeline_pause(s_pipe);
            set_state(PS_PAUSED);
            s_user_stopped = 0;  /* 标记为非用户主动暂停 */
            s_stuck_paused_since = esp_timer_get_time();
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* 设置 fallback DNS（校园网 DHCP 可能不返回 DNS 服务器） */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_dns_info_t dns_main = {};
            dns_main.ip.type = ESP_IPADDR_TYPE_V4;
            dns_main.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
            esp_netif_dns_info_t dns_backup = {};
            dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
            dns_backup.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
            ESP_LOGI(TAG, "DNS set: 8.8.8.8 (main), 114.114.114.114 (backup)");
        }
        /* WiFi 恢复后自动恢复播放 */
        if (s_pipe && get_state() == PS_PAUSED && !s_user_stopped) {
            ESP_LOGI(TAG, "WiFi restored, resuming playback");
            esp_gmf_pipeline_resume(s_pipe);
            set_state(PS_PLAYING);
            s_play_start_us = esp_timer_get_time();
        }
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

/* ─────────────────────── mDNS 服务发现 ─────────────────────── */
static void mdns_service_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    /* hostname 由 miplay_init() 统一设置为 device_id，这里不覆盖 */
    mdns_instance_name_set("MS01B");
    mdns_service_add(NULL, "_dlna", "_tcp", 8080, NULL, 0);
    ESP_LOGI(TAG, "mDNS: _dlna._tcp:8080 registered");
}

/* MiPlay 连接状态回调：暂停/恢复 DLNA SSDP */
static void dlna_on_miplay_connected(bool connected)
{
    ESP_LOGI(TAG, "=== MiPlay %s ===", connected ? "connected" : "disconnected");
    s_miplay_active = connected;
    custom_dlna_set_ssdp_suppressed(connected);
    if (!connected) {
        miplay_pipeline_stop();
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
        if (sta_netif) esp_netif_set_hostname(sta_netif, "ESP32-MIPLAY");
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(ESP_IF_WIFI_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_max_tx_power(78);

    /* ── mDNS 服务发现（WiFi 已连接）── */
    mdns_service_init();

    /* ── 封面 worker 任务（PSRAM 栈，先于音频创建） ── */
    album_art_worker_init();

    /* ── Now Playing 统一元数据层（DLNA/MiPlay 共用，须在协议启动前就绪） ── */
    np_init();
    np_set_meta_changed_cb(np_on_meta_changed);
    np_set_cover_url_cb(np_on_cover_url);
    np_set_source_changed_cb(np_on_source_changed);

    /* ── 音频播放 ── */
    audio_player_init();
    miplay_pipeline_init();

        
    /* ── DLNA 服务（SSDP + HTTP + SOAP）── */
    start_dlna();

    /* ── 旋钮 + 歌词按键 ── */
    rotary_encoder_setup();
    lyrics_btn_setup();

    /* ── TFT 显示 + LVGL ── */
    tft_init();
    lvgl_port_init(5);

    /* ── MiPlay 小米妙播（LVGL 之后，确保显示任务先分配堆内存）── */
    miplay_init();
    /* MiPlay 连接时暂停 DLNA SSDP，断开后恢复，避免手机混淆 */
    miplay_set_connected_cb(dlna_on_miplay_connected);

    /* 旋钮焦点框选 → 播放控制 */
    lvgl_port_ui_register_btn_prev_cb(cb_previous);
    lvgl_port_ui_register_btn_play_cb(cb_play_toggle);
    lvgl_port_ui_register_btn_next_cb(cb_next);

    lyrics_init();
    /* PSRAM 栈 + 12KB：LVGL 渲染/歌词/卡拉OK深调用链，4KB 内部 SRAM 栈过险
     * （对齐参考项目 DLNA_UI_UPDATE_STACK_BYTES=24KB，取保守 12KB） */
    dlna_create_task(ui_update_task, "ui_update", 24 * 1024, NULL, 3, NULL, 0);
}
