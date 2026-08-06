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
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "LVGL_UI";

/* 参考项目图片声明 */
LV_IMG_DECLARE(ui_img_haibao_png);
LV_IMG_DECLARE(ui_img_bofang1_png);
LV_IMG_DECLARE(ui_img_zanting1_png);
LV_IMG_DECLARE(ui_img_shangyi1_png);
LV_IMG_DECLARE(ui_img_xiyi1_png);
LV_IMG_DECLARE(ui_img_jiaopian_png);
LV_FONT_DECLARE(lv_font_simsun_16_cjk);

/* 控件 */
static lv_obj_t *s_cover_img;
static lv_img_dsc_t *s_cover_prev = NULL; /* 前一个动态封面，用于释放 */
#define COVER_BUF_SIZE (48 * 48)
static uint16_t s_cover_buf[COVER_BUF_SIZE]; /* 静态封面缓冲区（内部RAM，无缓存问题） */
static lv_obj_t *s_label_title;
static lv_obj_t *s_label_artist;
static lv_obj_t *s_bar_progress;
static lv_obj_t *s_label_time1;
static lv_obj_t *s_label_time2;
static lv_obj_t *s_btn_play;
static lv_obj_t *s_btn_next;
static lv_obj_t *s_btn_prev;

/* 主屏幕引用（用于切换回主界面） */
static lv_obj_t *s_main_scr = NULL;

/* 歌词界面 */
static lv_obj_t *s_lyrics_scr = NULL;
static lv_obj_t *s_lyrics_placeholder = NULL;
static lv_obj_t *s_lyrics_prev = NULL;    /* 上一句 */
static lv_obj_t *s_lyrics_curr = NULL;    /* 当前句 */
static lv_obj_t *s_lyrics_next = NULL;    /* 下一句 */
static int s_lyrics_current = -1;
static int s_lyrics_prev_line = -1;        /* 上一次的行号，用于检测切换动画 */
static bool s_lyrics_visible = false;
static lv_obj_t *s_lyrics_bg_img = NULL; /* 歌词界面背景图 */
/* 背景渐变（预抖动图片，消除 RGB565 色阶） */
static lv_obj_t *s_bg_img = NULL;        /* 背景图片控件 */
static lv_img_dsc_t *s_bg_dsc = NULL;    /* 背景图片描述符（PSRAM 像素数据） */
static void _generate_dithered_bg(uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                   uint8_t r_bot, uint8_t g_bot, uint8_t b_bot);

/* 当前句滚动状态（LV_LABEL_LONG_SCROLL 内部管理） */

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

    /* ====== 动态背景渐变（预抖动图片，消除 RGB565 色阶） ====== */
    s_bg_img = lv_img_create(scr);
    lv_obj_set_pos(s_bg_img, 0, 0);
    lv_obj_move_background(s_bg_img);
    _generate_dithered_bg(11, 40, 61, 4, 38, 62);   /* 默认深蓝渐变 */

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

    lvgl_port_ui_lyrics_create();
    s_main_scr = scr;  /* 保存主屏幕引用 */
    ESP_LOGI(TAG, "UI created");
}

/* ====== 歌词界面（3行：上一句/当前句/下一句） ====== */
void lvgl_port_ui_lyrics_create(void)
{
    s_lyrics_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(s_lyrics_scr, LV_OPA_0, 0);   /* 背景透明，由背景图显示 */
    lv_obj_clear_flag(s_lyrics_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 背景图（与主界面共享 s_bg_dsc 预抖动渐变） */
    s_lyrics_bg_img = lv_img_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_bg_img, 0, 0);
    lv_obj_move_background(s_lyrics_bg_img);
    if (s_bg_dsc) lv_img_set_src(s_lyrics_bg_img, s_bg_dsc);

    /* 暂无歌词占位（居中） */
    s_lyrics_placeholder = lv_label_create(s_lyrics_scr);
    lv_obj_set_width(s_lyrics_placeholder, 128);
    lv_obj_set_pos(s_lyrics_placeholder, 0, 72);
    lv_label_set_text(s_lyrics_placeholder, "暂无歌词");
    lv_obj_set_style_text_font(s_lyrics_placeholder, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_placeholder, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_placeholder, LV_TEXT_ALIGN_CENTER, 0);

    /* 上一句（Y=49，暗淡） */
    s_lyrics_prev = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_prev, 0, 49);
    lv_obj_set_width(s_lyrics_prev, 128);
    lv_label_set_text(s_lyrics_prev, "");
    lv_obj_set_style_text_font(s_lyrics_prev, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_prev, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_prev, LV_LABEL_LONG_CLIP);

    /* 当前句（Y=73，灰色+选中高亮，左对齐，缓慢左移） */
    s_lyrics_curr = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_curr, 0, 73);
    lv_obj_set_width(s_lyrics_curr, 128);
    lv_label_set_text(s_lyrics_curr, "");
    lv_obj_set_style_text_font(s_lyrics_curr, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_curr, C_DIM, 0);
    lv_obj_set_style_text_color(s_lyrics_curr, C_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_lyrics_curr, C_BG_TOP, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_lyrics_curr, LV_OPA_0, LV_PART_SELECTED);   /* 透明背景，让背景图透出 */
    lv_obj_set_style_text_align(s_lyrics_curr, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_lyrics_curr, LV_LABEL_LONG_CLIP);

    /* 下一句（Y=97，暗淡） */
    s_lyrics_next = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_next, 0, 97);
    lv_obj_set_width(s_lyrics_next, 128);
    lv_label_set_text(s_lyrics_next, "");
    lv_obj_set_style_text_font(s_lyrics_next, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_next, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_next, LV_LABEL_LONG_CLIP);

    ESP_LOGI(TAG, "Lyrics screen created (3-line)");
}

void lvgl_port_ui_toggle_lyrics(void)
{
    s_lyrics_visible = !s_lyrics_visible;
    if (s_lyrics_visible) {
        lv_scr_load(s_lyrics_scr);
    } else {
        if (s_main_scr) lv_scr_load(s_main_scr);
    }
}

bool lvgl_port_ui_lyrics_is_visible(void)
{
    return s_lyrics_visible;
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

/* ====== 生成预抖动渐变背景（Bayer 4×4 有序抖动，消除 RGB565 色阶） ====== */
static void _generate_dithered_bg(uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                   uint8_t r_bot, uint8_t g_bot, uint8_t b_bot)
{
    const int W = 128, H = 160;

    /* 首次调用分配 PSRAM 缓冲 + LVGL 图片描述符 */
    if (!s_bg_dsc) {
        s_bg_dsc = (lv_img_dsc_t *)lv_mem_alloc(sizeof(lv_img_dsc_t));
        if (!s_bg_dsc) return;
        memset(s_bg_dsc, 0, sizeof(lv_img_dsc_t));
        s_bg_dsc->header.always_zero = 0;
        s_bg_dsc->header.w = W;
        s_bg_dsc->header.h = H;
        s_bg_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
        s_bg_dsc->data_size = W * H * sizeof(uint16_t);
        s_bg_dsc->data = (const uint8_t *)heap_caps_malloc(s_bg_dsc->data_size, MALLOC_CAP_SPIRAM);
        if (!s_bg_dsc->data) {
            ESP_LOGW(TAG, "bg dither PSRAM alloc failed");
            lv_mem_free(s_bg_dsc);
            s_bg_dsc = NULL;
            return;
        }
    }

    uint16_t *buf = (uint16_t *)s_bg_dsc->data;

    /* 4x4 Bayer 矩阵 */
    static const uint8_t bayer[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5
    };

    for (int y = 0; y < H; y++) {
        int r8 = r_top + ((int)r_bot - r_top) * y / (H - 1);
        int g8 = g_top + ((int)g_bot - g_top) * y / (H - 1);
        int b8 = b_top + ((int)b_bot - b_top) * y / (H - 1);

        for (int x = 0; x < W; x++) {
            int t = bayer[(y & 3) * 4 + (x & 3)];

            /* 有序抖动量化到 RGB565 */
            int r5 = (r8 * 31 + t * 2 + 128) / 256;
            int g6 = (g8 * 63 + t * 4 + 128) / 256;
            int b5 = (b8 * 31 + t * 2 + 128) / 256;

            if (r5 < 0) r5 = 0; else if (r5 > 31) r5 = 31;
            if (g6 < 0) g6 = 0; else if (g6 > 63) g6 = 63;
            if (b5 < 0) b5 = 0; else if (b5 > 31) b5 = 31;

            /* BGR565 大端序（ST7735 MADCTL_BGR），与封面格式一致 */
            uint16_t c = (uint16_t)((b5 << 11) | (g6 << 5) | r5);
            buf[y * W + x] = (c >> 8) | (c << 8);
        }
    }

    lv_img_set_src(s_bg_img, s_bg_dsc);
    lv_obj_invalidate(s_bg_img);
    if (s_lyrics_bg_img) {
        lv_img_set_src(s_lyrics_bg_img, s_bg_dsc);
        lv_obj_invalidate(s_lyrics_bg_img);
    }
}

/* ====== 从封面 BGR565 大端序像素提取主色并更新背景 ====== */
static void _update_bg_from_cover(const uint16_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return;

    /* BGR565 大端序（ST7735 MADCTL_BGR）：
     * 内存布局: byte[0] = B5 + G3_high, byte[1] = G3_low + R5
     * uint16_t 小端读: [G2 G1 G0 R4 R3 R2 R1 R0] [B4 B3 B2 B1 B0 G5 G4 G3]
     *
     * R: bits 12-8 → (p >> 8) & 0x1F
     * G: bits 15-13, 2-0 → ((p >> 13) & 0x07) | ((p & 0x07) << 3)
     * B: bits 7-3  → (p >> 3) & 0x1F
     */
    unsigned long r_sum = 0, g_sum = 0, b_sum = 0;
    int count = w * h;

    for (int i = 0; i < count; i++) {
        uint16_t p = pixels[i];
        int r5 = (p >> 8) & 0x1F;
        int g6 = ((p >> 13) & 0x07) | ((p & 0x07) << 3);
        int b5 = (p >> 3) & 0x1F;
        r_sum += (r5 << 3) | (r5 >> 2);   /* 5→8 位扩展 */
        g_sum += (g6 << 2) | (g6 >> 4);   /* 6→8 位扩展 */
        b_sum += (b5 << 3) | (b5 >> 2);   /* 5→8 位扩展 */
    }

    if (count == 0) return;

    /* 暗化到 ~35% 作为背景主色 */
    uint8_t r = (uint8_t)((r_sum / count) * 90 / 256);
    uint8_t g = (uint8_t)((g_sum / count) * 90 / 256);
    uint8_t b = (uint8_t)((b_sum / count) * 90 / 256);

    /* 最小亮度保证 */
    if (r < 8 && g < 8 && b < 8) {
        r = 10; g = 10; b = 20;
    }

    /* 顶部稍亮，底部稍暗，形成极浅渐变 */
    uint8_t r_top = r + 6 > 255 ? 255 : r + 6;
    uint8_t g_top = g + 6 > 255 ? 255 : g + 6;
    uint8_t b_top = b + 8 > 255 ? 255 : b + 8;
    uint8_t r_bot = r > 6 ? r - 6 : 0;
    uint8_t g_bot = g > 6 ? g - 6 : 0;
    uint8_t b_bot = b > 8 ? b - 8 : 0;

    _generate_dithered_bg(r_top, g_top, b_top, r_bot, g_bot, b_bot);
}

static void _opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void lvgl_port_ui_lyrics_update(int current_idx, const char *prev, const char *curr, const char *next)
{
    /* 检测行号切换，触发向上滚动动画 */
    if (current_idx != s_lyrics_prev_line && s_lyrics_prev_line >= 0) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lyrics_prev);
        lv_anim_set_exec_cb(&a, _opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&a, 200);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lyrics_curr);
        lv_anim_set_exec_cb(&a, _opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 300);
        lv_anim_start(&a);
    }

    s_lyrics_current = current_idx;
    s_lyrics_prev_line = current_idx;

    lv_label_set_text(s_lyrics_prev, prev ? prev : "");
    lv_label_set_text(s_lyrics_curr, curr ? curr : "");
    lv_label_set_text(s_lyrics_next, next ? next : "");

    /* 上下行：超宽则左对齐，否则居中 */
    int16_t w_prev = lv_txt_get_width(prev ? prev : "", prev ? strlen(prev) : 0,
        lv_obj_get_style_text_font(s_lyrics_prev, 0), 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_align(s_lyrics_prev, w_prev > 128 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    int16_t w_next = lv_txt_get_width(next ? next : "", next ? strlen(next) : 0,
        lv_obj_get_style_text_font(s_lyrics_next, 0), 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_align(s_lyrics_next, w_next > 128 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    /* 当前行动态对齐：居中（放得下）或左对齐（超宽） */
    lv_font_t *font_curr = lv_obj_get_style_text_font(s_lyrics_curr, 0);
    int16_t w_curr = lv_txt_get_width(curr ? curr : "", curr ? strlen(curr) : 0,
        font_curr, 0, LV_TEXT_FLAG_NONE);
    bool curr_overflow = w_curr > 128;
    lv_obj_set_style_text_align(s_lyrics_curr,
        curr_overflow ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    /* 重置高亮及位置 */
    lv_label_set_text_sel_start(s_lyrics_curr, 0);
    lv_label_set_text_sel_end(s_lyrics_curr, 0);
    ((lv_label_t *)s_lyrics_curr)->offset.x = 0;

    /* 有歌词→隐藏占位 */
    if (curr && curr[0]) {
        if (s_lyrics_placeholder) lv_obj_add_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 逐字高亮（karaoke）+ 逐字左移 ── */
static int s_scroll_total = 0;   /* 总滚动距离（正数） */
static int s_text_bytes = 0;     /* 总字节数 */

void lvgl_port_ui_lyrics_karaoke(int byte_idx)
{
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) return;

    if (byte_idx <= 0) {
        lv_label_set_text_sel_start(s_lyrics_curr, 0);
        lv_label_set_text_sel_end(s_lyrics_curr, 0);
        ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
        return;
    }

    lv_label_set_text_sel_start(s_lyrics_curr, 0);
    lv_label_set_text_sel_end(s_lyrics_curr, byte_idx);

    /* 仅当文本超长时才左移，否则只高亮不移动 */
    if (s_scroll_total > 0) {
        /* 当前字保持在屏幕 1/4 位置，但不超出右边界 */
        int16_t w = lv_txt_get_width(text, byte_idx,
            lv_obj_get_style_text_font(s_lyrics_curr, 0), 0, LV_TEXT_FLAG_NONE);
        int label_w = lv_obj_get_width(s_lyrics_curr);
        int anchor = label_w / 4;
        int offset = 0;
        if (w > anchor) {
            offset = anchor - w;
        }
        /* 限制左移不超出右边界（最后一个字能在右边显示即可） */
        if (offset < -s_scroll_total) offset = -s_scroll_total;
        ((lv_label_t *)s_lyrics_curr)->offset.x = offset;
    }
}

void lvgl_port_ui_lyrics_set_scroll_dist(int dist)
{
    s_scroll_total = dist;
}

/* ── 当前句滚动状态 ── */
static int s_scroll_dist = 0;  /* 需滚动的像素（正数），0=不滚动 */

void lvgl_port_ui_lyrics_scroll_to_end(int line_duration_ms)
{
    (void)line_duration_ms;
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) { s_scroll_total = 0; s_text_bytes = 0; return; }

    /* 用 lv_txt_get_width 取实际像素宽度（不受 max_width 约束换行） */
    lv_font_t *font = lv_obj_get_style_text_font(s_lyrics_curr, 0);
    int16_t text_w = lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE);
    int label_w = lv_obj_get_width(s_lyrics_curr);
    if (text_w <= label_w) { s_scroll_total = 0; s_text_bytes = 0; return; }

    s_scroll_total = (int)text_w - label_w;
    s_text_bytes = strlen(text);
    ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
    ESP_LOGI(TAG, "scroll_dist: total=%d bytes=%d (text_w=%d label_w=%d)", s_scroll_total, s_text_bytes, text_w, label_w);
}

void lvgl_port_ui_lyrics_set_scroll_progress(int pct)
{
    if (s_scroll_dist <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int x = -s_scroll_dist * pct / 100;
    ((lv_label_t *)s_lyrics_curr)->offset.x = x;
    lv_obj_invalidate(s_lyrics_curr);
    if (pct % 25 == 0) ESP_LOGI(TAG, "scroll_x=%d (dist=%d pct=%d)", x, s_scroll_dist, pct);
}

void lvgl_port_ui_lyrics_tick_scroll(void)
{
    /* LV_LABEL_LONG_SCROLL 内部处理 */
}

void lvgl_port_ui_lyrics_clear(void)
{
    s_lyrics_current = -1;
    s_lyrics_prev_line = -1;
    lv_label_set_text(s_lyrics_prev, "");
    lv_label_set_text(s_lyrics_curr, "");
    lv_label_set_text(s_lyrics_next, "");
    lv_label_set_text_sel_start(s_lyrics_curr, 0);
    lv_label_set_text_sel_end(s_lyrics_curr, 0);
    s_scroll_total = 0;
    s_text_bytes = 0;
    if (s_lyrics_placeholder) lv_obj_clear_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Lyrics UI cleared");
}
void lvgl_port_ui_set_cover(const uint16_t *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return;
    size_t px_size = w * h * sizeof(uint16_t);
    if (px_size > sizeof(s_cover_buf)) {
        ESP_LOGW(TAG, "Cover too large: %dx%d", w, h);
        return;
    }

    ESP_LOGI(TAG, "set_cover: %dx%d", w, h);
    memcpy(s_cover_buf, pixels, px_size);
    _update_bg_from_cover(s_cover_buf, w, h);   /* 依据封面主色更新背景 */

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