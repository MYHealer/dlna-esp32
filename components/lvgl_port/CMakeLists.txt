idf_component_register(SRCS "lvgl_port_disp.c" "lvgl_port_indev.c" "lvgl_port_ui.c"
                    "ui_ref/ui_img_haibao_png.c" "ui_ref/ui_img_bofang1_png.c" "ui_ref/ui_img_zanting1_png.c"
                    "ui_ref/ui_img_shangyi1_png.c" "ui_ref/ui_img_xiyi1_png.c" "ui_ref/ui_img_jiaopian_png.c" "ui_ref/ui_img_bg_gradient.c"
                    INCLUDE_DIRS "include" "../lvgl" "ui_ref"
                    PRIV_REQUIRES tft_display rotary_encoder lvgl)