# Edit following two lines to set component requirements (see docs)
set(private_requires
        audio_sal
        audio_hal
        esp_dispatcher
        esp_peripherals
        display_service
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
)

idf_component_register(
        SRCS "board.c" "board_pins_config.c" "i2s_pa.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "prvi_include"
        PRIV_REQUIRES ${private_requires}
)

idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})