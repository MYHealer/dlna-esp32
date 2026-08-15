idf_component_register(
    SRCS
        esp_gmf_alc.c
        esp_gmf_audio_dec.c
        esp_gmf_audio_helper.c
        esp_gmf_rate_cvt.c
        esp_gmf_audio_param.c
    INCLUDE_DIRS "./include" "./include/esp_asrc" "./include/gmf_misc"
    PRIV_INCLUDE_DIRS "./private_include"
    REQUIRES esp_timer gmf_core esp-adf-libs
)
