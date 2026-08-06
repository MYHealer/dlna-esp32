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
                        esp_peripherals
                        audio_pipeline
                        audio_sal
                        audio_stream
                        custom_dlna
                        esp-adf-libs
                        esp_driver_jpeg
                        esp_http_client
                        esp-tls
                        esp_mm)

# Wrap audio_forge_init to increase default stack size (4KB -> 8KB)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=audio_forge_init")