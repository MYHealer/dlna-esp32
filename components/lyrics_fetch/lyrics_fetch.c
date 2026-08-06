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

/* 任务栈静态缓冲区（PSRAM），避免内部 RAM 分配失败 */
#define LYRIC_TASK_STACK_SIZE  8192
static StackType_t *s_task_stack;
static StaticTask_t s_task_tcb;

/* ── HTTP 响应缓冲 ── */
#define HTTP_SEARCH_BUF   2048
#define HTTP_LYRIC_BUF    8192

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
static int json_get_int(const char *json, jsmntok_t *tok)
{
    char tmp[16];
    int len = tok->end - tok->start;
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, json + tok->start, len);
    tmp[len] = '\0';
    return atoi(tmp);
}

/* ── 搜索歌曲，返回 songId，失败返回 0 ── */
static int search_song(const char *title, const char *artist)
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
    int best_id = 0;
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
        int song_id = 0;
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
            strncpy(best_name, song_name, sizeof(best_name) - 1);
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

/* ── 获取 LRC 歌词文本（调用方 free）── */
static char *fetch_lrc(int song_id)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://music.163.com/api/song/lyric?id=%d&lv=1", song_id);

    char *resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF);
    if (!resp) {
        ESP_LOGW(TAG, "fetch_lrc: HTTP request failed for song_id=%d", song_id);
        return NULL;
    }
    ESP_LOGI(TAG, "fetch_lrc: got response (%d bytes)", (int)strlen(resp));
    ESP_LOGI(TAG, "lyric resp: %.200s", resp);

    /* 解析 JSON: lrc.lyric（响应含 lrc/tlyric/romalrc，需要更多 tokens） */
    jsmn_parser parser;
    jsmntok_t tokens[256];
    jsmn_init(&parser);
    int num = jsmn_parse(&parser, resp, strlen(resp), tokens, 256);
    if (num < 0) {
        ESP_LOGE(TAG, "Lyric JSON parse error: %d", num);
        free(resp);
        return NULL;
    }

    int lrc_idx = json_find_key(resp, tokens, num, 0, "lrc");
    if (lrc_idx < 0) {
        ESP_LOGW(TAG, "No lrc field in response");
        free(resp);
        return NULL;
    }

    int lyric_idx = json_find_key(resp, tokens, num, lrc_idx, "lyric");
    if (lyric_idx < 0) {
        free(resp);
        return NULL;
    }

    int len = tokens[lyric_idx].end - tokens[lyric_idx].start;
    char *lrc = malloc(len + 1);
    if (!lrc) { free(resp); return NULL; }
    memcpy(lrc, resp + tokens[lyric_idx].start, len);
    lrc[len] = '\0';

    ESP_LOGI(TAG, "LRC extracted: %d bytes, first 80: '%.80s'", len, lrc);

    /* jsmn 不解码 JSON 转义，需要手动处理 \n → 换行 */
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (lrc[i] == '\\' && i + 1 < len && lrc[i + 1] == 'n') {
            lrc[j++] = '\n';
            i++;  /* 跳过 'n' */
        } else if (lrc[i] == '\\' && i + 1 < len && lrc[i + 1] == 'r') {
            lrc[j++] = '\r';
            i++;
        } else {
            lrc[j++] = lrc[i];
        }
    }
    lrc[j] = '\0';
    ESP_LOGI(TAG, "LRC after unescape: %d bytes, first 80: '%.80s'", j, lrc);

    free(resp);
    return lrc;
}

/* ── 解析 LRC 格式 "[MM:SS.xx]text" ── */
static void parse_lrc(const char *lrc_text, lyric_data_t *data)
{
    data->count = 0;
    data->loaded = false;

    const char *p = lrc_text;
    while (*p && data->count < LYRIC_MAX_LINES) {
        /* 跳过空行 */
        if (*p == '\n' || *p == '\r') { p++; continue; }

        /* 尝试解析时间标签 */
        if (*p == '[') {
            int mm = 0, ss = 0, ms = 0;
            /* 支持 [MM:SS.xx] 和 [MM:SS.xxx] */
            if (sscanf(p, "[%d:%d.%d]", &mm, &ss, &ms) >= 2) {
                /* 跳到 ']' 后面 */
                const char *text_start = strchr(p, ']');
                if (!text_start) { p++; continue; }
                text_start++;

                /* 计算毫秒 */
                int time_ms = mm * 60000 + ss * 1000;
                /* ms 可能是 2 位或 3 位 */
                if (ms < 100) ms *= 10;
                time_ms += ms;

                /* 提取歌词文本（到行尾） */
                char text[LYRIC_TEXT_LEN] = {0};
                int ti = 0;
                const char *t = text_start;
                while (*t && *t != '\n' && *t != '\r' && ti < LYRIC_TEXT_LEN - 1) {
                    text[ti++] = *t++;
                }
                text[ti] = '\0';

                /* 跳过空歌词行（纯音乐标记等） */
                if (ti > 0) {
                    /* 过滤元数据行（作词/作曲/编曲/制作人等） */
                    static const char *meta_keys[] = {
                        "作词", "作曲", "编曲", "制作人", "监制",
                        "录音", "混音", "母带", "吉他", "Bass",
                        "架子鼓", "大提琴", "小提琴", "管弦", "工程师",
                        "出品", "发行", "公司", "solo", "Studio",
                        NULL
                    };
                    bool is_meta = false;
                    for (int k = 0; meta_keys[k]; k++) {
                        if (strstr(text, meta_keys[k])) { is_meta = true; break; }
                    }
                    if (!is_meta) {
                        data->lines[data->count].time_ms = time_ms;
                        strncpy(data->lines[data->count].text, text, LYRIC_TEXT_LEN);
                        data->count++;
                    }
                }

                /* 跳到下一行 */
                p = t;
                while (*p == '\n' || *p == '\r') p++;
                continue;
            }
        }

        /* 非 LRC 行，跳过 */
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (data->count > 0) {
        data->loaded = true;
        ESP_LOGI(TAG, "Parsed %d lyric lines", data->count);
    }
}

/* ── 后台获取任务 ── */
static void fetch_task(void *arg)
{
    lyric_data_t *data = &s_lyric_data;
    char title[64], artist[64];
    int known_id;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(title, data->song_name, sizeof(title) - 1);
    strncpy(artist, data->artist, sizeof(artist) - 1);
    known_id = data->known_song_id;
    xSemaphoreGive(s_mutex);

    int song_id = known_id;

    if (song_id > 0) {
        ESP_LOGI(TAG, "Using known songId=%d for: %s - %s", song_id, title, artist);
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

void lyrics_fetch_async(const char *title, const char *artist, int song_id)
{
    ESP_LOGI(TAG, "lyrics_fetch_async called: '%s' - '%s' songId=%d", title, artist, song_id);
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

    /* 二分查找：找最后一个 time_ms <= position_ms 的行 */
    int lo = 0, hi = s_lyric_data.count - 1;
    int result = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_lyric_data.lines[mid].time_ms <= position_ms) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    xSemaphoreGive(s_mutex);
    return result;
}

const lyric_data_t *lyrics_get_data(void)
{
    return &s_lyric_data;
}

void lyrics_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    s_fetch_task = NULL;  /* 重置任务句柄，允许下次获取 */
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Lyrics cleared");
}
