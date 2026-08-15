idf_component_register(
    SRCS
        src/gmf_loader_setup_io.c
        src/gmf_loader_setup_audio_codec.c
        src/gmf_loader_setup_audio_effects.c
    INCLUDE_DIRS "./include"
    REQUIRES "mbedtls" "gmf_core" "gmf_io" "gmf_audio" "esp_codec_dev" "esp-adf-libs"
)
