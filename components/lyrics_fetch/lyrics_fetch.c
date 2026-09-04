/*
 * 多源歌词获取 — 网易云 → QQ 音乐 → 酷我 回退链
 *
 * 流程：歌名+歌手 → 依次尝试三源 → LRC 可用性校验 → 解析时间戳
 * 网易云: jsmn JSON 解析; QQ/酷我: 轻量字符串提取
 */

#include "lyrics_fetch.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "jsmn.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "LYRIC";

/* ── 静态数据 ── */
static lyric_data_t     s_lyric_data;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_fetch_task;

/* ── klyric 逐字时间戳（扁平数组，共 ~2.5KB）── */
#define KLYRIC_MAX_WORDS   256
static int  s_klyric_start[KLYRIC_MAX_WORDS];   /* 每个字的绝对起始毫秒 */
static int  s_klyric_end[KLYRIC_MAX_WORDS];     /* 每个字的绝对结束毫秒 */
static int  s_klyric_count;                     /* 总字数 */
static int  s_klyric_line_first[LYRIC_MAX_LINES]; /* 每行在 s_klyric_start 中的起始索引 */
static int  s_klyric_line_time[LYRIC_MAX_LINES];  /* 每行的起始时间（ms），用于按时间匹配 */
static int  s_klyric_line_count;                 /* 有 klyric 数据的行数 */
static int  s_lrc_to_klyric[LYRIC_MAX_LINES];   /* LRC行 → klyric行 映射，-1=无 */

/* 任务栈静态缓冲区（PSRAM），避免内部 RAM 分配失败 */
#define LYRIC_TASK_STACK_SIZE  8192
static StackType_t *s_task_stack;
static StaticTask_t s_task_tcb;
static TaskHandle_t s_dead_task;   /* 上一轮已退出待回收的 task，判 eDeleted 用 */

/* ── klyric 解析前向声明 ── */
static void parse_klyric(const char *klyric_text);

/* ── HTTP 响应缓冲 ── */
#define HTTP_SEARCH_BUF   2048
#define HTTP_LYRIC_BUF    32768

typedef struct {
    char    *buf;
    int      len;
    int      cap;
} resp_ctx_t;

/* ── URL 编码（UTF-8 安全）── */
static int url_encode(const char *src, char *dst, int dst_size)
{
    const char *hex = "0123456789ABCDEF";
    int di = 0;
    for (int i = 0; src[i] && di < dst_size - 3; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ── HTTP 事件回调：累积响应体 ── */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->buf[ctx->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── HTTP GET/POST，返回响应体（调用方 free）；referer 可为 NULL ── */
static char *http_request(const char *url, const char *post_data,
                          const char *content_type, int buf_size,
                          const char *referer)
{
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    resp_ctx_t ctx = { .buf = buf, .len = 0, .cap = buf_size };

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return NULL; }

    esp_http_client_set_header(client, "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120");
    esp_http_client_set_header(client, "Referer",
        referer ? referer : "https://music.163.com");

    esp_err_t err;
    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type",
            content_type ? content_type : "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        return NULL;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d, received=%d",
             status, content_length, ctx.len);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        free(buf);
        return NULL;
    }

    if (ctx.len == 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

/* ── jsmn 辅助：在 token 数组中查找 key ── */
static int json_find_key(const char *json, jsmntok_t *tokens, int num_tokens,
                         int start, const char *key)
{
    int key_len = strlen(key);
    for (int i = start; i < num_tokens; i++) {
        if (tokens[i].type == JSMN_STRING) {
            int len = tokens[i].end - tokens[i].start;
            if (len == key_len && memcmp(json + tokens[i].start, key, len) == 0) {
                return i + 1;  /* 返回 value 的 token 索引 */
            }
        }
        /* 跳过子树 */
        if (tokens[i].type == JSMN_OBJECT || tokens[i].type == JSMN_ARRAY) {
            int skip = tokens[i].size;
            for (int j = i + 1; j < num_tokens && skip > 0; j++) {
                if (tokens[j].type == JSMN_OBJECT || tokens[j].type == JSMN_ARRAY) {
                    skip += tokens[j].size;
                }
                skip--;
            }
            i += skip;
        }
    }
    return -1;
}

/* ── jsmn 辅助：提取 token 的字符串值 ── */
static int json_get_string(const char *json, jsmntok_t *tok, char *out, int out_size)
{
    int len = tok->end - tok->start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, json + tok->start, len);
    out[len] = '\0';
    return len;
}

/* ── jsmn 辅助：提取 token 的整数值 ── */
static unsigned long json_get_int(const char *json, jsmntok_t *tok)
{
    char tmp[24];
    int len = tok->end - tok->start;
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, json + tok->start, len);
    tmp[len] = '\0';
    return strtoul(tmp, NULL, 10);
}

/* ── 搜索歌曲，返回 songId，失败返回 0 ── */
static unsigned long search_song(const char *title, const char *artist)
{
    char keyword[128];
    snprintf(keyword, sizeof(keyword), "%s %s", title, artist);

    char encoded[256];
    url_encode(keyword, encoded, sizeof(encoded));

    char post_data[320];
    snprintf(post_data, sizeof(post_data),
             "s=%s&type=1&limit=1&offset=0", encoded);

    char *resp = http_request("http://music.163.com/api/search/get",
                              post_data, "application/x-www-form-urlencoded",
                              HTTP_SEARCH_BUF, NULL);
    if (!resp) {
        ESP_LOGW(TAG, "search_song: HTTP request failed");
        return 0;
    }
    ESP_LOGI(TAG, "search resp (%d bytes)", (int)strlen(resp));

    /* 解析 JSON */
    jsmn_parser parser;
    jsmntok_t tokens[512];
    jsmn_init(&parser);
    int num = jsmn_parse(&parser, resp, strlen(resp), tokens, 512);
    if (num < 0) {
        ESP_LOGE(TAG, "JSON parse error: %d", num);
        free(resp);
        return 0;
    }

    /* result.songs[] */
    int ri = json_find_key(resp, tokens, num, 0, "result");
    if (ri < 0) { free(resp); return 0; }
    int si = json_find_key(resp, tokens, num, ri, "songs");
    if (si < 0) { free(resp); return 0; }
    if (tokens[si].type != JSMN_ARRAY || tokens[si].size < 1) {
        free(resp); return 0;
    }

    /*
     * 遍历 songs 数组，对每个元素找顶层 "id" 和 "name"
     * 关键：跳过嵌套 object/array，避免匹配到 artist.id 等内部字段
     */
    int song_count = tokens[si].size;
    int pos = si + 1;  /* songs 数组第一个元素 */
    unsigned long best_id = 0;
    char best_name[64] = "";

    for (int s = 0; s < song_count && pos < num; s++) {
        if (tokens[pos].type != JSMN_OBJECT) { pos++; continue; }
        int obj_end = pos;
        /* 计算这个 object 的结束位置 */
        {
            int depth = 1;
            int j = pos + 1;
            while (j < num && depth > 0) {
                if (tokens[j].type == JSMN_OBJECT || tokens[j].type == JSMN_ARRAY) depth += tokens[j].size;
                j++;
                depth--;
            }
            obj_end = j;  /* object 结束后的下一个位置 */
        }

        /* 在这个 object 内找顶层 "id" 和 "name"（只看直接子 key） */
        unsigned long song_id = 0;
        char song_name[64] = "";
        int scan = pos + 1;
        while (scan < obj_end && scan < num) {
            if (tokens[scan].type == JSMN_STRING) {
                int klen = tokens[scan].end - tokens[scan].start;
                /* 顶层 "id" key */
                if (klen == 2 && memcmp(resp + tokens[scan].start, "id", 2) == 0
                    && scan + 1 < num) {
                    song_id = json_get_int(resp, &tokens[scan + 1]);
                }
                /* 顶层 "name" key */
                if (klen == 4 && memcmp(resp + tokens[scan].start, "name", 4) == 0
                    && scan + 1 < num && tokens[scan + 1].type == JSMN_PRIMITIVE) {
                    /* 跳过（可能是嵌套 object 的 name） */
                }
                if (klen == 4 && memcmp(resp + tokens[scan].start, "name", 4) == 0
                    && scan + 1 < num && tokens[scan + 1].type == JSMN_STRING) {
                    json_get_string(resp, &tokens[scan + 1], song_name, sizeof(song_name));
                }
                /* 跳过 value */
                scan += 2;
                /* 如果 value 是 object/array，跳过子树 */
                if (scan - 1 < num && (tokens[scan - 1].type == JSMN_OBJECT || tokens[scan - 1].type == JSMN_ARRAY)) {
                    int skip_depth = tokens[scan - 1].size;
                    while (scan < num && skip_depth > 0) {
                        if (tokens[scan].type == JSMN_OBJECT || tokens[scan].type == JSMN_ARRAY)
                            skip_depth += tokens[scan].size;
                        scan++;
                        skip_depth--;
                    }
                }
            } else {
                scan++;
            }
        }

        ESP_LOGI(TAG, "song[%d]: id=%d name='%s'", s, song_id, song_name);
        if (song_id > 0 && best_id == 0) {
            best_id = song_id;
            snprintf(best_name, sizeof(best_name), "%s", song_name);
        }

        pos = obj_end;
    }

    if (best_id > 0) {
        ESP_LOGI(TAG, "Found: [%d] %s", best_id, best_name);
    } else {
        ESP_LOGW(TAG, "Song not found");
    }

    free(resp);
    return best_id;
}

/* ── JSON 转义还原（\n → 换行等）── */
static void json_unescape(char *buf, int len)
{
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\\' && i + 1 < len && buf[i + 1] == 'n') {
            buf[j++] = '\n';
            i++;
        } else if (buf[i] == '\\' && i + 1 < len && buf[i + 1] == 'r') {
            buf[j++] = '\r';
            i++;
        } else {
            buf[j++] = buf[i];
        }
    }
    buf[j] = '\0';
}

/* ── 从 JSON 中提取指定字段的文本（调用方 free）── */
static char *extract_json_field(const char *resp, jsmntok_t *tokens, int num_tokens,
                                 const char *obj_key, const char *field_key)
{
    int idx = json_find_key(resp, tokens, num_tokens, 0, obj_key);
    if (idx < 0) return NULL;
    int fld = json_find_key(resp, tokens, num_tokens, idx, field_key);
    if (fld < 0) return NULL;
    int len = tokens[fld].end - tokens[fld].start;
    if (len <= 0) return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, resp + tokens[fld].start, len);
    out[len] = '\0';
    json_unescape(out, len);
    return out;
}

/* ── 获取 LRC + klyric 歌词文本（调用方 free 返回的 LRC）── */
static char *fetch_lrc(unsigned long song_id)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://music.163.com/api/song/lyric?id=%lu&lv=-1&kv=-1&tv=-1&yv=-1", song_id);

    char *resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF, NULL);
    if (!resp) {
        ESP_LOGW(TAG, "fetch_lrc: HTTP request failed for song_id=%lu", song_id);
        return NULL;
    }
    ESP_LOGI(TAG, "fetch_lrc: got response (%d bytes)", (int)strlen(resp));

    jsmn_parser parser;
    jsmntok_t *tokens = (jsmntok_t *)heap_caps_malloc(4096 * sizeof(jsmntok_t), MALLOC_CAP_SPIRAM);
    if (!tokens) {
        ESP_LOGE(TAG, "Failed to alloc jsmn tokens from PSRAM");
        free(resp);
        return NULL;
    }
    jsmn_init(&parser);
    int num = jsmn_parse(&parser, resp, strlen(resp), tokens, 4096);
    if (num < 0) {
        ESP_LOGE(TAG, "Lyric JSON parse error: %d", num);
        free(tokens);
        free(resp);
        return NULL;
    }

    char *lrc = extract_json_field(resp, tokens, num, "lrc", "lyric");
    if (!lrc) {
        ESP_LOGW(TAG, "No lrc field in response");
        free(tokens);
        free(resp);
        return NULL;
    }
    ESP_LOGI(TAG, "LRC extracted: %d bytes, first 80: '%.80s'", (int)strlen(lrc), lrc);

    static const char *klyric_keys[] = {"yrc", "klyric", "yrcxl", NULL};
    for (int kk = 0; klyric_keys[kk]; kk++) {
        char *klyric_raw = extract_json_field(resp, tokens, num, klyric_keys[kk], "lyric");
        if (klyric_raw && strlen(klyric_raw) > 10) {
            ESP_LOGI(TAG, "klyric(%s) extracted: %d bytes, first 80: '%.80s'",
                klyric_keys[kk], (int)strlen(klyric_raw), klyric_raw);
            parse_klyric(klyric_raw);
            free(klyric_raw);
            if (s_klyric_count > 0) break;  /* 有逐字数据才跳出 */
        } else if (klyric_raw) {
            ESP_LOGI(TAG, "klyric(%s) too short (%d bytes), skip", klyric_keys[kk], (int)strlen(klyric_raw));
            free(klyric_raw);
        }
    }

    free(tokens);
    free(resp);
    return lrc;
}

/* ── 从 LRC 行数据生成近似逐字时间戳（无 klyric 时的回退）──
 * 策略：按行时长均分到每个字，标点处额外停顿 */
static void generate_pseudo_klyric(const lyric_data_t *data)
{
    if (s_klyric_count > 0 || !data || data->count < 2) return;

    ESP_LOGI(TAG, "Generating pseudo-klyric from %d LRC lines", data->count);

    int word_idx = 0;
    int line_idx = 0;

    for (int i = 0; i < data->count && word_idx < KLYRIC_MAX_WORDS - 1 && line_idx < LYRIC_MAX_LINES; i++) {
        const char *text = data->lines[i].text;
        if (!text[0]) continue;

        int line_start = data->lines[i].time_ms;
        /* 末句无下句时，按可唱字符数估算时长（参考 MeloX min/max 0.32s/字, 2~8s）
         * 避免短末句固定5秒长时间空滚 */
        int line_end;
        if (i + 1 < data->count) {
            line_end = data->lines[i + 1].time_ms;
        } else {
            int sung_chars = 0;
            const char *st = text;
            while (*st) {
                unsigned char c = (unsigned char)*st;
                if (c >= 0xE0) { sung_chars++; st += 3; }
                else if (c >= 0xC0) { sung_chars++; st += 2; }
                else { sung_chars++; st++; }
            }
            int est_ms = (int)(sung_chars * 320.0f);   /* 0.32s/字 */
            if (est_ms < 2000) est_ms = 2000;          /* 下限 2s */
            if (est_ms > 8000) est_ms = 8000;          /* 上限 8s */
            line_end = line_start + est_ms;
        }
        int line_dur = line_end - line_start;
        if (line_dur <= 0) line_dur = 3000;

        /* 统计"权重"：中文=1，英文=0.5，标点=0.3 */
        float total_weight = 0;
        const char *p = text;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c >= 0xE0) { total_weight += 1.0f; p += 3; }       /* CJK 3字节 */
            else if (c >= 0xC0) { total_weight += 1.0f; p += 2; }  /* 2字节 UTF-8 */
            else {
                if (c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
                    c == ';' || c == ':' || c == '-' || c == '，' || c == '。' ||
                    c == '！' || c == '？' || c == '；' || c == '、' || c == '…') {
                    total_weight += 0.3f;
                } else {
                    total_weight += 0.5f;  /* ASCII 字母 */
                }
                p++;
            }
        }
        if (total_weight < 1) total_weight = 1;

        s_klyric_line_first[line_idx] = word_idx;
        s_klyric_line_time[line_idx] = line_start;
        line_idx++;

        /* 逐字分配时间 */
        float time_per_weight = (float)line_dur / total_weight;
        int t = line_start;
        p = text;
        while (*p && word_idx < KLYRIC_MAX_WORDS - 1) {
            unsigned char c = (unsigned char)*p;
            int char_bytes = 1;
            float w;
            if (c >= 0xE0) { char_bytes = 3; w = 1.0f; }
            else if (c >= 0xC0) { char_bytes = 2; w = 1.0f; }
            else {
                if (c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
                    c == ';' || c == ':' || c == '-' || c == '，' || c == '。' ||
                    c == '！' || c == '？' || c == '；' || c == '、' || c == '…') {
                    w = 0.3f;
                } else {
                    w = 0.5f;
                }
            }
            int dur = (int)(w * time_per_weight);
            if (dur < 50) dur = 50;

            s_klyric_start[word_idx] = t;
            s_klyric_end[word_idx] = t + dur;
            word_idx++;
            t += dur;

            p += char_bytes;
        }
    }

    s_klyric_count = word_idx;
    s_klyric_line_count = line_idx;
    ESP_LOGI(TAG, "Pseudo-klyric: %d words across %d lines", s_klyric_count, s_klyric_line_count);
}

/* ── 解析 LRC 格式 "[MM:SS.xx]text" ── */
static void parse_lrc(const char *lrc_text, lyric_data_t *data)
{
    data->count = 0;
    data->loaded = false;

    const char *p = lrc_text;
    while (*p && data->count < LYRIC_MAX_LINES) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        if (*p == '[') {
            int mm = 0, ss = 0, ms = 0;
            if (sscanf(p, "[%d:%d.%d]", &mm, &ss, &ms) >= 2) {
                const char *text_start = strchr(p, ']');
                if (!text_start) { p++; continue; }
                text_start++;

                int time_ms = mm * 60000 + ss * 1000;
                if (ms < 100) ms *= 10;
                time_ms += ms;

                char text[LYRIC_TEXT_LEN] = {0};
                int ti = 0;
                const char *t = text_start;
                while (*t && *t != '\n' && *t != '\r' && ti < LYRIC_TEXT_LEN - 1) {
                    text[ti++] = *t++;
                }
                text[ti] = '\0';

                if (ti > 0) {
                    /* 强过滤元数据行：作词/作曲/编曲/翻译/歌词来源等 */
                    static const char *meta_keys[] = {
                        "作词", "作曲", "编曲", "制作人", "监制",
                        "录音", "混音", "母带", "吉他", "Bass",
                        "架子鼓", "大提琴", "小提琴", "管弦", "工程师",
                        "出品", "发行", "公司", "solo", "Studio",
                        "词曲", "词：", "曲：", "Lyrics", "lyrics",
                        "Composer", "Arranger", "Producer", "Written",
                        "混缩", "和声", "OP:", "SP:", "OP：", "SP：",
                        "来源", "翻译", "译配", "改编",
                        NULL
                    };
                    bool is_meta = false;
                    for (int k = 0; meta_keys[k]; k++) {
                        if (strstr(text, meta_keys[k])) { is_meta = true; break; }
                    }
                    /* 含冒号的行（"词曲 : XXX"、"Lyrics by: XXX"） */
                    if (!is_meta && strchr(text, ':')) is_meta = true;
                    /* 纯空白或极短 */
                    if (!is_meta && ti <= 2) is_meta = true;
                    if (!is_meta) {
                        data->lines[data->count].time_ms = time_ms;
                        strncpy(data->lines[data->count].text, text, LYRIC_TEXT_LEN);
                        data->count++;
                    }
                }

                p = t;
                while (*p == '\n' || *p == '\r') p++;
                continue;
            }
        }

        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (data->count > 0) {
        data->loaded = true;
        ESP_LOGI(TAG, "Parsed %d lyric lines", data->count);
    }
}

/* ── 解析 klyric/yrc 逐字时间戳 ──
 * klyric 格式: [line_start,dur](offset,dur)word...  （offset 相对前一字末尾）
 * yrc 格式:    [line_start,dur](start,dur,0)word...  （start 绝对毫秒）
 */
static void parse_klyric(const char *klyric_text)
{
    s_klyric_count = 0;
    s_klyric_line_count = 0;

    const char *p = klyric_text;
    while (*p && s_klyric_count < KLYRIC_MAX_WORDS && s_klyric_line_count < LYRIC_MAX_LINES) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        if (*p == '[') {
            int line_start = 0, line_dur = 0;
            if (sscanf(p, "[%d,%d]", &line_start, &line_dur) >= 2) {
                s_klyric_line_first[s_klyric_line_count] = s_klyric_count;
                s_klyric_line_time[s_klyric_line_count] = line_start;
                s_klyric_line_count++;

                const char *rp = strchr(p, ']');
                if (!rp) { p++; continue; }
                rp++;

                /* yrc 格式用绝对时间，klyric 格式用相对偏移 */
                bool is_yrc = false;
                int word_time = line_start;
                while (*rp && *rp != '\n' && *rp != '\r' && s_klyric_count < KLYRIC_MAX_WORDS) {
                    if (*rp != '(') { rp++; continue; }

                    int a = 0, b = 0, c = 0;
                    int n = sscanf(rp, "(%d,%d,%d)", &a, &b, &c);
                    if (n == 3) {
                        /* yrc 格式: (start,dur,flag) — start 是绝对毫秒 */
                        if (!is_yrc) {
                            is_yrc = true;
                            word_time = line_start;
                        }
                        /* dur<=0 时兜底（对齐 LyricOn: dur==0 → end-start，
                         * 保证每个字至少有点时长，karaoke 不会瞬间跳过） */
                        int end = a + b;
                        if (b <= 0) {
                            if (end > a) b = end - a;
                            else { b = 1; end = a + 1; }
                        }
                        s_klyric_start[s_klyric_count] = a;
                        s_klyric_end[s_klyric_count] = end;
                        s_klyric_count++;
                        rp = strchr(rp, ')');
                        if (!rp) break;
                        rp++;
                        while (*rp && *rp != '(' && *rp != '\n' && *rp != '\r') rp++;
                    } else if (sscanf(rp, "(%d,%d)", &a, &b) >= 2) {
                        /* klyric 格式: (offset,dur) — offset 相对前一字末尾 */
                        if (!is_yrc) {
                            word_time += a;
                            int end = word_time + b;
                            if (b <= 0) {   /* dur<=0 兜底，避免 0 时长字 */
                                if (end > word_time) b = end - word_time;
                                else { b = 1; end = word_time + 1; }
                            }
                            s_klyric_start[s_klyric_count] = word_time;
                            s_klyric_end[s_klyric_count] = word_time + b;
                            s_klyric_count++;
                            word_time += b;
                        } else {
                            /* yrc 中也可能有 2 参数格式，跳过 */
                        }
                        rp = strchr(rp, ')');
                        if (!rp) break;
                        rp++;
                        while (*rp && *rp != '(' && *rp != '\n' && *rp != '\r') rp++;
                    } else {
                        rp++;
                    }
                }
            }
        }

        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }

    ESP_LOGI(TAG, "Parsed %d klyric words across %d lines (yrc=%s)",
             s_klyric_count, s_klyric_line_count,
             s_klyric_count > 0 && s_klyric_start[0] >= s_klyric_line_time[0] ? "yes" : "no");
}

/* ── 后台获取任务 ── */

/* ══════════ 多源回退：通用辅助 ══════════ */

/* ── 标题清洗：截掉括号后缀，提高搜索命中率 ──
 * 手机推送的标题常带翻译/说明: "君の笑顔... (你的笑颜是最爱)"、"歌名 (Live)"。
 * 后缀噪音会让搜索 miss；截掉括号保留主标题。 */
static void clean_title_for_search(const char *src, char *dst, int dst_size)
{
    int cut = -1;
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '(') { cut = i; break; }
        /* 全角左括号 （ = U+FF08 = EF BC 88 */
        if (c == 0xEF && (unsigned char)src[i + 1] == 0xBC &&
            (unsigned char)src[i + 2] == 0x88) {
            cut = i; break;
        }
    }
    if (cut < 0) {
        snprintf(dst, dst_size, "%s", src);
        return;
    }
    while (cut > 0 && src[cut - 1] == ' ') cut--;   /* 去尾随空格 */
    if (cut > dst_size - 1) cut = dst_size - 1;
    memcpy(dst, src, cut);
    dst[cut] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ── 轻量 JSON 字符串提取 + 标准转义还原（调用方 free；找不到返回 NULL）──
 * QQ/酷我接口字段浅且无嵌套同名 key，字符串扫描足够；比 jsmn 省 token 数组 */
static char *json_extract_string_alloc(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        /* 宽松匹配: "key": " "（带空格） */
        snprintf(pat, sizeof(pat), "\"%s\": \"", key);
        p = strstr(json, pat);
    }
    if (!p) return NULL;
    p += strlen(pat);

    /* 计算值长度（处理 \" 转义） */
    int len = 0;
    while (p[len] && p[len] != '"') {
        if (p[len] == '\\' && p[len + 1]) len++;
        len++;
    }
    if (len == 0) return NULL;

    char *out = malloc(len + 1);
    if (!out) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (p[i] == '\\' && i + 1 < len) {
            char c = p[i + 1];
            if (c == 'n') { out[j++] = '\n'; i++; }
            else if (c == 'r') { out[j++] = '\r'; i++; }
            else if (c == 't') { out[j++] = '\t'; i++; }
            else if (c == '"') { out[j++] = '"'; i++; }
            else if (c == '\\') { out[j++] = '\\'; i++; }
            else if (c == '/') { out[j++] = '/'; i++; }
            else if (c == 'u' && i + 4 < len) {
                int cp = 0;
                bool ok = true;
                for (int k = 1; k <= 4; k++) {
                    int hv = hex_val(p[i + 1 + k]);
                    if (hv < 0) { ok = false; break; }
                    cp = cp * 16 + hv;
                }
                if (ok) {
                    i += 5;
                    /* BMP → UTF-8（代理对/非BMP 歌词场景极少，忽略） */
                    if (cp < 0x80) out[j++] = (char)cp;
                    else if (cp < 0x800) {
                        out[j++] = (char)(0xC0 | (cp >> 6));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[j++] = (char)(0xE0 | (cp >> 12));
                        out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    out[j++] = p[i];
                }
            } else {
                out[j++] = p[i];
            }
        } else {
            out[j++] = p[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ── base64 解码（调用方 free，结果以 NUL 结尾）── */
static char *base64_decode_alloc(const char *src)
{
    if (!src) return NULL;
    size_t slen = strlen(src);
    if (slen == 0) return NULL;

    size_t dlen = 0;
    /* 尺寸查询：dst=NULL 时返回所需长度（非法字符时 dlen 保持 0） */
    mbedtls_base64_decode(NULL, 0, &dlen, (const unsigned char *)src, slen);
    if (dlen == 0) return NULL;

    unsigned char *out = malloc(dlen + 1);
    if (!out) return NULL;

    size_t olen = 0;
    int ret = mbedtls_base64_decode(out, dlen, &olen,
                                    (const unsigned char *)src, slen);
    if (ret != 0 || olen == 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return (char *)out;
}

/* ── LRC 可用性校验：≥2 个时间戳 + 排除占位文本 ── */
static bool lrc_usable(const char *lrc)
{
    if (!lrc) return false;

    int stamps = 0;
    for (const char *p = lrc; *p; p++) {
        if (*p == '[' && p[1] >= '0' && p[1] <= '9') {
            int mm = 0, ss = 0;
            if (sscanf(p, "[%d:%d]", &mm, &ss) == 2) {
                stamps++;
                p += 2;
            }
        }
    }
    if (stamps < 2) return false;

    static const char *placeholders[] = {
        "纯音乐", "暂无歌词", "暂无歌词!", "没有填词", "没有歌词",
        "nolyric", "No lyrics", "此歌曲为没有填词的纯音乐", NULL
    };
    for (int i = 0; placeholders[i]; i++) {
        if (strstr(lrc, placeholders[i])) return false;
    }
    return true;
}

/* ── 歌词载荷归一化：已是 LRC 直接返回，否则尝试 base64 解码（调用方 free）── */
static char *normalize_lyric_payload(char *raw)
{
    if (!raw) return NULL;
    if (lrc_usable(raw)) return raw;

    char *dec = base64_decode_alloc(raw);
    free(raw);
    if (dec && lrc_usable(dec)) return dec;
    free(dec);
    return NULL;
}

/* ══════════ QQ 音乐源 ══════════ */

/* ── QQ 音乐搜索+获取歌词（返回 LRC，调用方 free）──
 * 注：用 HTTP 而非 HTTPS — MiPlay 播放中内部 RAM 紧张，mbedtls TLS 上下文
 * 分配失败（esp-aes: Failed to allocate memory）；HTTP 可绕开。 */
static char *fetch_lrc_qq(const char *title, const char *artist)
{
    char clean[64];
    clean_title_for_search(title, clean, sizeof(clean));

    char keyword[128];
    snprintf(keyword, sizeof(keyword), "%s %s", clean, artist);
    char encoded[256];
    url_encode(keyword, encoded, sizeof(encoded));

    /* 1. 搜索拿 songmid */
    char url[384];
    snprintf(url, sizeof(url),
             "http://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=%s&format=json&n=1",
             encoded);
    char *resp = http_request(url, NULL, NULL, HTTP_SEARCH_BUF, "https://y.qq.com");
    if (!resp) {
        ESP_LOGW(TAG, "qq search: HTTP failed");
        return NULL;
    }

    char *songmid = json_extract_string_alloc(resp, "songmid");
    free(resp);
    if (!songmid || !songmid[0]) {
        ESP_LOGW(TAG, "qq search: no songmid");
        free(songmid);
        return NULL;
    }
    ESP_LOGI(TAG, "qq: songmid=%s", songmid);

    /* 2. 获取歌词 */
    snprintf(url, sizeof(url),
             "http://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=%s&format=json&nobase64=1",
             songmid);
    free(songmid);

    resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF, "https://y.qq.com");
    if (!resp) {
        ESP_LOGW(TAG, "qq lyric: HTTP failed");
        return NULL;
    }

    char *raw = json_extract_string_alloc(resp, "lyric");
    free(resp);
    if (!raw) {
        ESP_LOGW(TAG, "qq lyric: no lyric field");
        return NULL;
    }

    /* nobase64=1 有时仍返回 base64，normalize 兜底 */
    char *lrc = normalize_lyric_payload(raw);
    if (lrc) {
        ESP_LOGI(TAG, "qq: LRC ok (%d bytes)", (int)strlen(lrc));
    } else {
        ESP_LOGW(TAG, "qq: LRC unusable");
    }
    return lrc;
}

/* ══════════ 酷我源 ══════════ */

/* ── 酷我 lrclist JSON → LRC 文本（调用方 free）──
 * 输入形如: {"lrclist":[{"timeA":"00:20.53","lineTxt":"..."},...]} */
static char *kuwo_lrclist_to_lrc(const char *json)
{
    const char *arr = strstr(json, "\"lrclist\"");
    if (!arr) return NULL;

    /* 粗略容量：每行预留 96 字节 */
    int cap = 96 * LYRIC_MAX_LINES;
    char *out = malloc(cap);
    if (!out) return NULL;
    int oi = 0;

    const char *p = arr;
    while ((p = strstr(p, "\"timeA\"")) != NULL && oi < cap - 128) {
        const char *ta = strstr(p, ":");
        if (!ta) break;
        ta++;
        while (*ta == ' ' || *ta == '"') ta++;
        char time_str[16] = {0};
        int ti = 0;
        while (*ta && *ta != '"' && ti < 15) time_str[ti++] = *ta++;
        time_str[ti] = '\0';

        const char *lt = strstr(ta, "\"lineTxt\"");
        const char *next_time = strstr(ta, "\"timeA\"");
        if (!lt || (next_time && lt > next_time)) { p = ta; continue; }
        lt = strstr(lt, ":");
        if (!lt) break;
        lt++;
        while (*lt == ' ' || *lt == '"') lt++;

        oi += snprintf(out + oi, cap - oi, "[%s]", time_str);
        while (*lt && *lt != '"' && oi < cap - 4) {
            if (*lt == '\\' && lt[1]) { lt++; }
            out[oi++] = *lt++;
        }
        out[oi++] = '\n';
        p = lt;
    }

    if (oi == 0) { free(out); return NULL; }
    out[oi] = '\0';
    return out;
}

/* ── 酷我搜索+获取歌词（返回 LRC，调用方 free）── */
static char *fetch_lrc_kuwo(const char *title, const char *artist)
{
    char clean[64];
    clean_title_for_search(title, clean, sizeof(clean));

    char keyword[128];
    snprintf(keyword, sizeof(keyword), "%s %s", clean, artist);
    char encoded[256];
    url_encode(keyword, encoded, sizeof(encoded));

    /* 1. 搜索拿 MUSICRID（形如 "MUSIC_123456"） */
    char url[384];
    snprintf(url, sizeof(url),
             "http://search.kuwo.cn/r.s?all=%s&ft=music&rformat=json&encoding=utf8&rn=1",
             encoded);
    char *resp = http_request(url, NULL, NULL, HTTP_SEARCH_BUF, NULL);
    if (!resp) {
        ESP_LOGW(TAG, "kuwo search: HTTP failed");
        return NULL;
    }

    const char *mp = strstr(resp, "MUSIC_");
    unsigned long music_id = 0;
    if (mp) {
        music_id = strtoul(mp + 6, NULL, 10);
    }
    free(resp);
    if (music_id == 0) {
        ESP_LOGW(TAG, "kuwo search: no MUSICRID");
        return NULL;
    }
    ESP_LOGI(TAG, "kuwo: musicId=%lu", music_id);

    /* 2. 获取歌词 */
    snprintf(url, sizeof(url),
             "http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=%lu",
             music_id);
    resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF, NULL);
    if (!resp) {
        ESP_LOGW(TAG, "kuwo lyric: HTTP failed");
        return NULL;
    }

    char *lrc = kuwo_lrclist_to_lrc(resp);
    if (!lrc) {
        /* 诊断：lrclist 缺失时打出响应头部，区分"无歌词"与"接口变了" */
        ESP_LOGW(TAG, "kuwo: no lrclist, resp head: %.120s", resp);
    }
    free(resp);
    if (lrc && lrc_usable(lrc)) {
        ESP_LOGI(TAG, "kuwo: LRC ok (%d bytes)", (int)strlen(lrc));
        return lrc;
    }
    free(lrc);
    ESP_LOGW(TAG, "kuwo: LRC unusable");
    return NULL;
}

/* ══════════ 应用歌词（解析 + klyric 映射）══════════ */

/* ── 校验通过的 LRC 应用到数据区（锁内）── */
static void apply_lrc(const char *lrc, lyric_data_t *data)
{
    parse_lrc(lrc, data);
    if (s_klyric_count == 0 && data->count > 0) {
        generate_pseudo_klyric(data);
    }
    /* 构建 LRC行 → klyric行 映射 */
    memset(s_lrc_to_klyric, -1, sizeof(s_lrc_to_klyric));
    if (s_klyric_line_count > 0 && data->count > 0) {
        int ki = 0;
        for (int li = 0; li < data->count; li++) {
            int lrc_time = data->lines[li].time_ms;
            while (ki < s_klyric_line_count - 1 &&
                   abs(s_klyric_line_time[ki + 1] - lrc_time) < abs(s_klyric_line_time[ki] - lrc_time)) {
                ki++;
            }
            if (ki < s_klyric_line_count && abs(s_klyric_line_time[ki] - lrc_time) <= 100) {
                s_lrc_to_klyric[li] = ki;
            }
        }
    }
}

/* ══════════ 后台获取任务 ══════════ */

/* ── 三源回退获取（网易云 → QQ → 酷我），返回可用 LRC（调用方 free）── */
static char *fetch_lrc_multisource(const char *title, const char *artist,
                                   unsigned long known_id)
{
    /* 网易云（有 known_id 时免搜索）；搜索时用清洗后标题 */
    unsigned long netease_id = known_id;
    if (netease_id == 0) {
        char clean[64];
        clean_title_for_search(title, clean, sizeof(clean));
        netease_id = search_song(clean, artist);
    }
    if (netease_id > 0) {
        char *lrc = fetch_lrc(netease_id);
        if (lrc && lrc_usable(lrc)) return lrc;
        free(lrc);
        ESP_LOGW(TAG, "netease unusable, falling back to QQ");
    } else {
        ESP_LOGW(TAG, "netease search miss, falling back to QQ");
    }

    /* 源2: QQ 音乐 */
    char *lrc = fetch_lrc_qq(title, artist);
    if (lrc) return lrc;

    /* 源3: 酷我 */
    ESP_LOGW(TAG, "qq unusable, falling back to kuwo");
    return fetch_lrc_kuwo(title, artist);
}

/* ── 后台获取任务 ── */
static void fetch_task(void *arg)
{
    lyric_data_t *data = &s_lyric_data;
    char title[64], artist[64];
    unsigned long known_id;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(title, sizeof(title), "%s", data->song_name);
    snprintf(artist, sizeof(artist), "%s", data->artist);
    known_id = data->known_song_id;
    xSemaphoreGive(s_mutex);

    unsigned long song_id = known_id;

    if (song_id > 0) {
        ESP_LOGI(TAG, "Using known songId=%lu for: %s - %s", song_id, title, artist);
    } else {
        ESP_LOGI(TAG, "Searching lyrics: %s - %s", title, artist);
    }

    /* 步骤1-2：三源回退获取可用 LRC（网易云→QQ→酷我） */
    char *lrc = fetch_lrc_multisource(title, artist, known_id);
    if (!lrc) {
        ESP_LOGW(TAG, "No lyrics available from any source");
        goto done;
    }

    /* 步骤3：解析 LRC（含伪逐字生成 + 行映射） */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_lrc(lrc, data);
    xSemaphoreGive(s_mutex);

    free(lrc);

done:
    /* 保存句柄供下一轮判活（s_fetch_task 置 NULL 是允许新请求的信号） */
    s_dead_task = s_fetch_task;
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

/* ── 公共 API ── */

esp_err_t lyrics_init(void)
{
    if (s_mutex) return ESP_OK;  /* 已初始化 */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    ESP_LOGI(TAG, "Lyrics component initialized");
    return ESP_OK;
}

void lyrics_fetch_async(const char *title, const char *artist, unsigned long song_id)
{
    ESP_LOGI(TAG, "lyrics_fetch_async called: '%s' - '%s' songId=%lu", title, artist, song_id);
    if (!s_mutex) lyrics_init();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 缓存命中：歌名+歌手相同且已加载 */
    if (s_lyric_data.loaded &&
        strcmp(s_lyric_data.song_name, title) == 0 &&
        strcmp(s_lyric_data.artist, artist) == 0) {
        ESP_LOGI(TAG, "Lyrics cached for: %s - %s", title, artist);
        xSemaphoreGive(s_mutex);
        return;
    }

    /* 清除旧歌词 */
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    strncpy(s_lyric_data.song_name, title, sizeof(s_lyric_data.song_name) - 1);
    strncpy(s_lyric_data.artist, artist, sizeof(s_lyric_data.artist) - 1);
    s_lyric_data.known_song_id = song_id;
    xSemaphoreGive(s_mutex);

    /* 如果已有任务在跑，不重复创建 */
    if (s_fetch_task) {
        ESP_LOGW(TAG, "Fetch already in progress");
        return;
    }

    /* 从 PSRAM 分配任务栈，避免内部 RAM 不足 */
    if (!s_task_stack) {
        s_task_stack = (StackType_t *)heap_caps_malloc(LYRIC_TASK_STACK_SIZE,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_task_stack) {
            ESP_LOGE(TAG, "Failed to allocate task stack from PSRAM");
            return;
        }
        ESP_LOGI(TAG, "Task stack allocated from PSRAM: %u bytes", (unsigned)LYRIC_TASK_STACK_SIZE);
    }

    /* 关键：等待旧 fetch task 真正被回收后再复用静态栈/TCB。
     * vTaskDelete(NULL) 是异步的——task 要等 idle task 跑回收后才变 eDeleted。
     * 若立即重建，新 task 与旧 task 残骸共用同一块栈 → 栈内容被踩 →
     * 堆元数据损坏 → tlsf assert 随机崩溃（实测切歌后第二次投歌触发）。
     * 判据同 miplay.c:3522：eTaskGetState(old) == eDeleted。 */
    if (s_dead_task) {
        for (int wt = 0; wt < 200 && eTaskGetState(s_dead_task) != eDeleted; wt++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_dead_task = NULL;
    }

    s_fetch_task = xTaskCreateStaticPinnedToCore(
        fetch_task, "lyric_fetch", LYRIC_TASK_STACK_SIZE,
        NULL, 3, s_task_stack, &s_task_tcb, 1);
    ESP_LOGI(TAG, "Fetch task created (static, core 1)");
}

int lyrics_get_current_line(int position_ms)
{
    if (!s_mutex) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_lyric_data.loaded || s_lyric_data.count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 线性扫描：找最后一行 time_ms <= position_ms（参考 LrcView 写法） */
    int result = -1;
    for (int i = 0; i < s_lyric_data.count; i++) {
        if (s_lyric_data.lines[i].time_ms <= position_ms) {
            result = i;
        }
    }

    xSemaphoreGive(s_mutex);
    return result;
}

const lyric_data_t *lyrics_get_data(void)
{
    return &s_lyric_data;
}

int lyrics_get_karaoke_progress(int line_start_ms, int pos_ms)
{
    if (!s_mutex) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_klyric_line_count == 0 || s_klyric_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 按行起始时间匹配 klyric 行，不依赖行索引（LRC/klyric 行数可能不一致） */
    int kline = -1;
    for (int i = 0; i < s_klyric_line_count; i++) {
        if (s_klyric_line_time[i] <= line_start_ms + 50) {
            kline = i;
        }
    }
    if (kline < 0 || abs(s_klyric_line_time[kline] - line_start_ms) > 100) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    int first = s_klyric_line_first[kline];
    int last = (kline + 1 < s_klyric_line_count)
        ? s_klyric_line_first[kline + 1]
        : s_klyric_count;
    int word_count = last - first;
    if (word_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 二分查找当前所处的字 */
    int lo = first, hi = last - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_klyric_end[mid] <= pos_ms) {
            lo = mid + 1;
        } else {
            found = mid;
            hi = mid - 1;
        }
    }

    int progress;
    if (found < 0) {
        progress = 100;
    } else if (pos_ms < s_klyric_start[found]) {
        int idx = found - first;
        progress = (idx > 0) ? idx * 100 / word_count : 0;
    } else {
        int idx = found - first;
        int word_dur = s_klyric_end[found] - s_klyric_start[found];
        if (word_dur <= 0) {
            progress = (idx + 1) * 100 / word_count;
        } else {
            int sub = (pos_ms - s_klyric_start[found]) * 100 / word_dur;
            if (sub > 100) sub = 100;
            progress = (idx * 100 + sub) / word_count;
        }
    }

    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    xSemaphoreGive(s_mutex);
    return progress;
}

/* ── 获取逐字高亮的精确字节偏移（百分比映射 + 字间插值）──
 * 核心思路：用 klyric 百分比映射到 LRC 全部字符，不依赖 word_count 一致性 */
int lyrics_get_karaoke_byte_idx(int line_idx, const char *line_text, int pos_ms)
{
    if (!s_mutex || !line_text || !line_text[0] || line_idx < 0) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_klyric_line_count == 0 || s_klyric_count == 0 || line_idx >= LYRIC_MAX_LINES) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 直接查映射表 */
    int kline = s_lrc_to_klyric[line_idx];
    if (kline < 0 || kline >= s_klyric_line_count) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    int first = s_klyric_line_first[kline];
    int last = (kline + 1 < s_klyric_line_count)
        ? s_klyric_line_first[kline + 1]
        : s_klyric_count;
    int word_count = last - first;
    if (word_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 移植 MeloX 语义：不用百分比二次映射，直接看逐字时间戳决定"已唱到第几个字"。
     * 每个字独立的 [start,end]，pos_ms 进入该字区间即点亮该字。
     * 已唱字数 = 处于或越过本行第几个字。 */
    int sung_words;
    int found = -1;
    for (int k = first; k < last; k++) {
        if (pos_ms >= s_klyric_start[k]) {
            found = k;
        } else {
            break;
        }
    }
    sung_words = (found < 0) ? 0 : (found - first + 1);
    if (sung_words > word_count) sung_words = word_count;

    xSemaphoreGive(s_mutex);

    /* 统计 line_text 中的可唱字符总数 */
    int total_sung = 0;
    const char *t = line_text;
    while (*t) {
        unsigned char c = (unsigned char)*t;
        bool is_sung = true;
        int bytes = 1;
        if (c >= 0xE0) {
            bytes = 3;
            if (t[0] == (char)0xEF && t[1] == (char)0xBC) {
                unsigned char c2 = (unsigned char)t[2];
                if (c2 >= 0x81 && c2 <= 0x9F) is_sung = false;
                if (c2 >= 0xA1 && c2 <= 0xB0) is_sung = false;
            }
            if (t[0] == (char)0xE3 && t[1] == (char)0x80) {
                unsigned char c2 = (unsigned char)t[2];
                if (c2 >= 0x80 && c2 <= 0x8F) is_sung = false;
            }
        } else if (c >= 0xC0) {
            bytes = 2;
        } else {
            if (c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
                c == ';' || c == ':' || c == '-' || c == '\'' || c == '"') {
                is_sung = false;
            }
        }
        if (is_sung) total_sung++;
        t += bytes;
    }

    /* 逐字严格对应：一个字 = 一个可唱字符（klyric 每个括号一个字）。
     * 已唱字数即已点亮字符数。当前正唱到的字也视为亮起半个取其整，
     * 让高亮覆盖到"正在唱"的那个字。 */
    int target_char = sung_words;
    if (target_char > total_sung) target_char = total_sung;
    const char *p = line_text;
    int char_count = 0;
    while (*p && char_count < target_char) {
        unsigned char c = (unsigned char)*p;
        bool is_sung = true;
        int bytes = 1;
        if (c >= 0xE0) {
            bytes = 3;
            if (p[0] == (char)0xEF && p[1] == (char)0xBC) {
                unsigned char c2 = (unsigned char)p[2];
                if (c2 >= 0x81 && c2 <= 0x9F) is_sung = false;
                if (c2 >= 0xA1 && c2 <= 0xB0) is_sung = false;
            }
            if (p[0] == (char)0xE3 && p[1] == (char)0x80) {
                unsigned char c2 = (unsigned char)p[2];
                if (c2 >= 0x80 && c2 <= 0x8F) is_sung = false;
            }
        } else if (c >= 0xC0) {
            bytes = 2;
        } else {
            if (c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
                c == ';' || c == ':' || c == '-' || c == '\'' || c == '"') {
                is_sung = false;
            }
        }
        if (is_sung) char_count++;
        p += bytes;
    }

    return (int)(p - line_text);
}

void lyrics_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    s_klyric_count = 0;
    s_klyric_line_count = 0;
    memset(s_lrc_to_klyric, -1, sizeof(s_lrc_to_klyric));
    s_fetch_task = NULL;  /* 重置任务句柄，允许下次获取 */
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Lyrics cleared");
}
