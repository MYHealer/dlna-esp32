idf_component_register(SRCS "rotary_encoder.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_timer esp_driver_pcnt esp_driver_gpio)
