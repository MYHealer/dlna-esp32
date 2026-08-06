/*
 * 网易云歌词获取 — 从 ESP32 直接调网易云 API
 *
 * 流程：歌名+歌手 → 搜索拿 songId → 获取 LRC 歌词 → 解析时间戳
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LYRIC_MAX_LINES    128        /* 最大歌词行数 */
#define LYRIC_TEXT_LEN      64        /* 每行最大字符 */

typedef struct {
    int  time_ms;                      /* 时间戳（毫秒） */
    char text[LYRIC_TEXT_LEN];         /* 歌词文本 */
} lyric_line_t;

typedef struct {
    lyric_line_t lines[LYRIC_MAX_LINES];
    int  count;                        /* 有效行数 */
    bool loaded;                       /* 是否已加载 */
    char song_name[64];                /* 当前歌曲名（缓存 key） */
    char artist[64];                   /* 当前歌手（缓存 key） */
    int  known_song_id;                /* 已知 songId（>0 跳过搜索） */
} lyric_data_t;

/**
 * @brief 初始化歌词组件
 */
esp_err_t lyrics_init(void);

/**
 * @brief 异步获取歌词（会创建后台任务）
 * @param title    歌曲名
 * @param artist   歌手名
 * @param song_id  网易云 songId（>0 时跳过搜索，直接获取歌词）
 */
void lyrics_fetch_async(const char *title, const char *artist, int song_id);

/**
 * @brief 根据播放位置获取当前歌词行索引
 * @param position_ms 当前播放位置（毫秒）
 * @return 歌词行索引，-1 表示无歌词或未加载
 */
int lyrics_get_current_line(int position_ms);

/**
 * @brief 获取歌词数据（只读指针）
 */
const lyric_data_t *lyrics_get_data(void);

/**
 * @brief 清除歌词缓存（切歌时调用）
 */
void lyrics_clear(void);

#ifdef __cplusplus
}
#endif
