/*
 * LVGL UI — 128x160 竖屏，参考项目风格
 * 所有容器关闭滚动条，封面居中于唱片盘
 */

#include "lvgl_port.h"
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LVGL_UI";

/* 参考项目图片声明 */
LV_IMG_DECLARE(ui_img_haibao_png);
LV_IMG_DECLARE(ui_img_bofang1_png);
LV_IMG_DECLARE(ui_img_zanting1_png);
LV_IMG_DECLARE(ui_img_shangyi1_png);
LV_IMG_DECLARE(ui_img_xiyi1_png);
LV_IMG_DECLARE(ui_img_jiaopian_png);
LV_IMG_DECLARE(ui_img_bg_gradient);
LV_FONT_DECLARE(lv_font_simsun_16_cjk);

/* 控件 */
static lv_obj_t *s_cover_img;
static lv_img_dsc_t *s_cover_prev = NULL; /* 前一个动态封面，用于释放 */
#define COVER_BUF_SIZE (48 * 48)
static uint16_t s_cover_buf[COVER_BUF_SIZE]; /* 静态封面缓冲区（内部RAM，无缓存问题） */
static lv_obj_t *s_label_title;
static lv_obj_t *s_label_artist;
static lv_obj_t *s_label_lyric;
static lv_obj_t *s_bar_progress;
static lv_obj_t *s_label_time1;
static lv_obj_t *s_label_time2;
static lv_obj_t *s_btn_play;
static lv_obj_t *s_btn_next;
static lv_obj_t *s_btn_prev;

/* 配色 */
#define C_TOP_BAR  lv_color_hex(0x021F33)
#define C_BG_TOP   lv_color_hex(0x0B283D)
#define C_BG_MID   lv_color_hex(0x263D4D)
#define C_BG_BOT   lv_color_hex(0x04263E)
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_WHITE80  lv_color_hex(0xCCCCCC)
#define C_DIM      lv_color_hex(0x506070)
#define C_ACCENT   lv_color_hex(0x00CCFF)
#define C_GREEN    lv_color_hex(0x00FF00)
#define C_BTN_BG   lv_color_hex(0x1A3A50)

void lvgl_port_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 关闭屏幕滚动条 */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* ====== 预渲染背景渐变（消除 RGB565 色阶） ====== */
    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, &ui_img_bg_gradient);
    lv_obj_set_pos(bg, 0, 0);

    /* ====== 唱片转盘 ====== */
    /* jiaopian 169x164, zoom=121 → 79x77 像素 */
    lv_obj_t *disc = lv_img_create(scr);
    lv_img_set_src(disc, &ui_img_jiaopian_png);
    lv_obj_set_pos(disc, 24, 2);
    lv_img_set_zoom(disc, 121);
    lv_img_set_pivot(disc, 0, 0);

    /* ====== 封面（中心对齐转盘） ====== */
    s_cover_img = lv_img_create(scr);
    lv_obj_set_pos(s_cover_img, 40, 17);
    lv_img_set_src(s_cover_img, &ui_img_haibao_png);
    lv_img_set_zoom(s_cover_img, 132);
    lv_img_set_pivot(s_cover_img, 0, 0);
    lv_obj_set_style_radius(s_cover_img, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_cover_img, true, 0);
    lv_obj_set_style_border_width(s_cover_img, 0, 0);

    /* ====== 标题 ====== */
    s_label_title = lv_label_create(scr);
    lv_obj_set_pos(s_label_title, 0, 78);
    lv_obj_set_width(s_label_title, 128);
    lv_label_set_text(s_label_title, "DLNA Player");
    lv_obj_set_style_text_color(s_label_title, C_WHITE, 0);
    lv_obj_set_style_text_font(s_label_title, &lv_font_simsun_16_cjk, 0);
    lv_label_set_long_mode(s_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(s_label_title, 30, 0);
    lv_obj_set_style_text_align(s_label_title, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 歌手 ====== */
    s_label_artist = lv_label_create(scr);
    lv_obj_set_pos(s_label_artist, 0, 96);
    lv_obj_set_width(s_label_artist, 128);
    lv_label_set_text(s_label_artist, "Waiting...");
    lv_obj_set_style_text_color(s_label_artist, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_artist, &lv_font_simsun_16_cjk, 0);
    lv_label_set_long_mode(s_label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(s_label_artist, 30, 0);
    lv_obj_set_style_text_align(s_label_artist, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 歌词 ====== */
    s_label_lyric = lv_label_create(scr);
    lv_obj_set_pos(s_label_lyric, 0, 106);
    lv_obj_set_size(s_label_lyric, 128, 10);
    lv_label_set_text(s_label_lyric, "");
    lv_obj_set_style_text_color(s_label_lyric, C_WHITE, 0);
    lv_obj_set_style_text_font(s_label_lyric, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_label_lyric, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_label_lyric, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* ====== 进度条 + 时间 ====== */
    s_bar_progress = lv_bar_create(scr);
    lv_obj_set_pos(s_bar_progress, 14, 116);
    lv_obj_set_size(s_bar_progress, 100, 2);
    lv_bar_set_range(s_bar_progress, 0, 1000);
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_progress, C_BTN_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_progress, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    s_label_time1 = lv_label_create(scr);
    lv_obj_set_pos(s_label_time1, 14, 119);
    lv_obj_set_size(s_label_time1, 45, 12);
    lv_label_set_text(s_label_time1, "0:00");
    lv_obj_set_style_text_color(s_label_time1, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_time1, &lv_font_montserrat_12, 0);

    s_label_time2 = lv_label_create(scr);
    lv_obj_set_pos(s_label_time2, 69, 119);
    lv_obj_set_size(s_label_time2, 45, 12);
    lv_label_set_text(s_label_time2, "0:00");
    lv_obj_set_style_text_color(s_label_time2, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_time2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_label_time2, LV_TEXT_ALIGN_RIGHT, 0);

    /* ====== 控制按钮 ====== */
    s_btn_prev = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_prev, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_shangyi1_png, NULL);
    lv_obj_set_pos(s_btn_prev, 16, 129);
    lv_obj_set_size(s_btn_prev, 31, 31);

    s_btn_play = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_bofang1_png, NULL);
    lv_obj_set_pos(s_btn_play, 48, 129);
    lv_obj_set_size(s_btn_play, 31, 31);

    s_btn_next = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_next, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_xiyi1_png, NULL);
    lv_obj_set_pos(s_btn_next, 80, 129);
    lv_obj_set_size(s_btn_next, 31, 31);

    ESP_LOGI(TAG, "UI created");
}

/* ====== UI 更新 ====== */
void lvgl_port_ui_set_title(const char *title) {
    lv_label_set_text(s_label_title, title ? title : "DLNA Player");
}
void lvgl_port_ui_set_artist(const char *artist) {
    lv_label_set_text(s_label_artist, artist && artist[0] ? artist : "Unknown");
}
void lvgl_port_ui_set_progress(int position_sec, int duration_sec) {
    int pmin = position_sec / 60, psec = position_sec % 60;
    if (duration_sec > 0) {
        int dmin = duration_sec / 60, dsec = duration_sec % 60;
        int permille = position_sec * 1000 / duration_sec;
        if (permille > 1000) permille = 1000;
        lv_bar_set_value(s_bar_progress, permille, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_label_time1, "%d:%02d", pmin, psec);
        lv_label_set_text_fmt(s_label_time2, "%d:%02d", dmin, dsec);
    } else {
        lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_label_time1, "%d:%02d", pmin, psec);
        lv_label_set_text(s_label_time2, "0:00");
    }
}
void lvgl_port_ui_set_state(int state) {
    if (state == 1) {
        lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_zanting1_png, NULL);
    } else {
        lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_bofang1_png, NULL);
    }
}
void lvgl_port_ui_set_volume(int vol) { (void)vol; }
void lvgl_port_ui_set_lyric_line(int index, const char *text) {
    if (index == 2) lv_label_set_text(s_label_lyric, text && text[0] ? text : "");
}
void lvgl_port_ui_set_lyric_current(int idx) { (void)idx; }
void lvgl_port_ui_set_cover(const uint16_t *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return;
    size_t px_size = w * h * sizeof(uint16_t);
    if (px_size > sizeof(s_cover_buf)) {
        ESP_LOGW(TAG, "Cover too large: %dx%d", w, h);
        return;
    }

    ESP_LOGI(TAG, "set_cover: %dx%d", w, h);
    memcpy(s_cover_buf, pixels, px_size);

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_mem_alloc(sizeof(lv_img_dsc_t));
    if (!dsc) return;
    dsc->header.always_zero = 0;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = px_size;
    dsc->data = (const uint8_t *)s_cover_buf;
    lv_img_set_src(s_cover_img, dsc);
    lv_obj_invalidate(s_cover_img);
    /* 48x48 原尺寸显示，居中于转盘 */
    lv_img_set_zoom(s_cover_img, 256);
    lv_img_set_pivot(s_cover_img, 0, 0);
    lv_obj_set_pos(s_cover_img, 64 - w / 2, 41 - h / 2);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    if (s_cover_prev) lv_mem_free(s_cover_prev);
    s_cover_prev = dsc;
}