/*
 * Now Playing — 薄接口层：元数据存储 + 回调通知
 *
 * 不做任何 I/O（不下载封面、不解码图片、不获取歌词）。
 * 协议层（DLNA/MiPlay）调用 np_set_meta / np_set_cover_url 存储数据，
 * UI 层（dlna.c）通过回调接收通知，用自己的逻辑处理。
 *
 * 参考 FusionPlay 架构：source 标签 + epoch 计数器，
 * 防止旧协议事件污染新会话。
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 来源协议（参考 FusionPlay MediaSource）── */
typedef enum {
    NP_SRC_NONE = 0,
    NP_SRC_DLNA,
    NP_SRC_MIPLAY,
} np_source_t;

/* ── 元数据 ── */
typedef struct {
    char        title[128];
    char        artist[64];
    char        album[64];
    uint32_t    duration_ms;
    np_source_t source;     /* 来自哪个协议 */
    uint32_t    epoch;      /* 会话代次，切换协议时递增 */
    /* 封面引用：HTTP URL，或 "data:<mime>;base64,xxx"（内嵌 base64，UI 层识别） */
    char        cover_url[512];
    bool        has_cover;  /* 是否有封面可抓/可解 */
} np_meta_t;

/**
 * @brief 统一提交：源 + 元数据 + 封面 一次上报
 *        内部：若源变化先 np_set_source（清屏+epoch++），再存 meta 触发回调
 *        供 DLNA / MiPlay 协议层解析歌曲信息后调用（参考 FusionPlay emit_media_info）
 */
void np_submit(np_source_t src, const np_meta_t *meta);

/**
 * @brief 设置当前播放来源（协议切换时调用）
 *        递增 epoch，清除旧元数据，触发清屏回调
 */
void np_set_source(np_source_t src);

/**
 * @brief 获取当前来源
 */
np_source_t np_get_source(void);

/**
 * @brief 获取当前 epoch
 */
uint32_t np_get_epoch(void);

/**
 * @brief 设置当前播放元数据（协议层调用）
 *        存储后触发 meta_changed 回调
 */
void np_set_meta(const np_meta_t *meta);

/**
 * @brief 读取当前播放元数据（UI 层调用）
 */
const np_meta_t *np_get_meta(void);

/* ── 封面 URL ── */

/**
 * @brief 设置封面 URL（协议层调用）
 *        存储后触发 cover_url 回调，由 UI 层接管下载
 */
void np_set_cover_url(const char *url);

/**
 * @brief 读取当前封面 URL
 */
const char *np_get_cover_url(void);

/* ── 回调（UI 层注册）── */
typedef void (*np_meta_changed_cb_t)(const np_meta_t *meta);
typedef void (*np_cover_url_cb_t)(const char *url);
typedef void (*np_source_changed_cb_t)(np_source_t src);

void np_set_meta_changed_cb(np_meta_changed_cb_t cb);
void np_set_cover_url_cb(np_cover_url_cb_t cb);
void np_set_source_changed_cb(np_source_changed_cb_t cb);

/* ── 清除所有状态（协议切换时调用）── */
void np_clear(void);

/* ── 初始化（零内存分配）── */
esp_err_t np_init(void);

#ifdef __cplusplus
}
#endif
