idf_component_register(SRCS "lyrics_fetch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client jsmn esp-tls)
