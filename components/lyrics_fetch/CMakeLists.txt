idf_component_register(SRCS "lyrics_fetch.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client jsmn esp-tls)

# Suppress truncation warnings (strncpy/snprintf on variable-length strings)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-stringop-truncation)
