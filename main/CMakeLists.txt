idf_component_register(SRCS "dlna.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES ../logo.png
                    PRIV_REQUIRES
                        rotary_encoder
                        tft_display
                        lvgl
                        lvgl_port
                        lyrics_fetch
                        nvs_flash
                        i2s_pa
                        custom_dlna
                        esp_codec_dev
                        gmf_core
                        gmf_io
                        gmf_audio
                        gmf_loader
                        esp-adf-libs
                        esp_http_client
                        esp-tls
                        esp_mm
                        esp_netif
                        esp_wifi
                        espressif__mdns
                        miplay
                        now_playing
                        webp)
