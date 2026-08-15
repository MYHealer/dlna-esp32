/*
 * 网易云歌词获取 — ESP32 直接调网易云音乐 API
 *
 * 流程：歌名+歌手 → 搜索拿 songId → 获取 LRC 歌词 → 解析时间戳
 * 使用 esp_http_client + jsmn JSON 解析
 */

#include "lyrics_fetch.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "jsmn.h"
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

/* ── HTTP GET/POST，返回响应体（调用方 free）── */
static char *http_request(const char *url, const char *post_data,
                          const char *content_type, int buf_size)
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
    esp_http_client_set_header(client, "Referer", "https://music.163.com");

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
                              HTTP_SEARCH_BUF);
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

    char *resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF);
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
                        s_klyric_start[s_klyric_count] = a;
                        s_klyric_end[s_klyric_count] = a + b;
                        s_klyric_count++;
                        rp = strchr(rp, ')');
                        if (!rp) break;
                        rp++;
                        while (*rp && *rp != '(' && *rp != '\n' && *rp != '\r') rp++;
                    } else if (sscanf(rp, "(%d,%d)", &a, &b) >= 2) {
                        /* klyric 格式: (offset,dur) — offset 相对前一字末尾 */
                        if (!is_yrc) {
                            word_time += a;
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
        song_id = search_song(title, artist);
        if (song_id == 0) {
            ESP_LOGW(TAG, "Song not found");
            goto done;
        }
    }

    /* 步骤2：获取 LRC */
    char *lrc = fetch_lrc(song_id);
    if (!lrc) {
        ESP_LOGW(TAG, "No lyrics available");
        goto done;
    }

    /* 步骤3：解析 LRC */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    parse_lrc(lrc, data);
    /* 步骤3.5：无逐字数据时，从 LRC 生成近似逐字时间戳 */
    if (s_klyric_count == 0 && data->count > 0) {
        generate_pseudo_klyric(data);
    }
    /* 步骤3.6：构建 LRC行 → klyric行 映射 */
    memset(s_lrc_to_klyric, -1, sizeof(s_lrc_to_klyric));
    if (s_klyric_line_count > 0 && data->count > 0) {
        int ki = 0;
        for (int li = 0; li < data->count; li++) {
            int lrc_time = data->lines[li].time_ms;
            /* 找最匹配的 klyric 行 */
            while (ki < s_klyric_line_count - 1 &&
                   abs(s_klyric_line_time[ki + 1] - lrc_time) < abs(s_klyric_line_time[ki] - lrc_time)) {
                ki++;
            }
            if (ki < s_klyric_line_count && abs(s_klyric_line_time[ki] - lrc_time) <= 100) {
                s_lrc_to_klyric[li] = ki;
            }
        }
    }
    xSemaphoreGive(s_mutex);

    free(lrc);

done:
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
