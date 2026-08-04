idf_component_register(SRCS "dlna.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES ../logo.png
                    PRIV_REQUIRES
                        rotary_encoder
                        nvs_flash
                        i2s_pa
                        esp_peripherals
                        audio_pipeline
                        audio_sal
                        audio_stream
                        custom_dlna
                        esp-adf-libs)