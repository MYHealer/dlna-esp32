idf_component_register(
    SRCS "miplay.c"
    INCLUDE_DIRS "include"
    REQUIRES mdns lwip esp_wifi esp_netif esp_timer mbedtls
             esp_codec_dev i2s_pa esp-adf-libs esp_driver_gpio esp_ringbuf
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
