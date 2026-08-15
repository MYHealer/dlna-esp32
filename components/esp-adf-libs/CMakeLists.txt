# ESP Audio Codec + 辅助库（精简版）
# 只保留项目实际使用的子组件，剔除 ADF 遗留的全量打包

set(TARGET ${CONFIG_IDF_TARGET})

# ── 头文件路径 ──
set(INCLUDES
    ./esp_audio_codec/include
    ./esp_audio_codec/include/simple_dec
    ./esp_audio_codec/include/encoder
    ./esp_audio_codec/include/encoder/impl
    ./esp_audio_codec/include/decoder
    ./esp_audio_codec/include/decoder/impl
    ./esp_new_jpeg/include
    ./esp_muxer/include
    ./esp_muxer/include/impl
    ./esp_audio_effects/include
    ./media_lib_sal/include
    ./media_lib_sal/include/port
)

# ── 源文件（media_lib_sal OS 抽象 + codec 注册） ──
set(SRCS
    ./media_lib_sal/media_lib_common.c
    ./media_lib_sal/media_lib_os.c
    ./media_lib_sal/mem_trace/media_lib_mem_trace.c
    ./media_lib_sal/mem_trace/media_lib_mem_his.c
    ./media_lib_sal/port/media_lib_os_freertos.c
    ./media_lib_sal/media_lib_adapter.c
    ./esp_audio_codec/src/audio_decoder_reg.c
    ./esp_audio_codec/src/simple_decoder_reg.c
)

idf_component_register(
    INCLUDE_DIRS ${INCLUDES}
    SRCS ${SRCS}
    PRIV_REQUIRES nghttp esp-tls esp_netif freertos
)

target_compile_options(${COMPONENT_TARGET} PRIVATE
    -Wno-calloc-transposed-args
    -Wno-error=absolute-value
)

# ── 预编译库 ──
add_prebuilt_library(esp_audio_codec_lib
    "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_codec/lib/${TARGET}/libesp_audio_codec.a"
    PRIV_REQUIRES esp-adf-libs)

add_prebuilt_library(esp_audio_simple_dec
    "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_codec/lib/${TARGET}/libesp_audio_simple_dec.a"
    PRIV_REQUIRES esp-adf-libs)

add_prebuilt_library(esp_new_jpeg
    "${CMAKE_CURRENT_SOURCE_DIR}/esp_new_jpeg/lib/${TARGET}/libesp_new_jpeg.a"
    PRIV_REQUIRES esp-adf-libs)

add_prebuilt_library(esp_muxer
    "${CMAKE_CURRENT_SOURCE_DIR}/esp_muxer/lib/${TARGET}/libesp_muxer.a"
    PRIV_REQUIRES esp-adf-libs)

add_prebuilt_library(esp_audio_effects
    "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_effects/lib/${TARGET}/libesp_audio_effects.a"
    PRIV_REQUIRES esp-adf-libs)

# ── 链接 ──
# --allow-multiple-definition: audio_codec 和 effects 可能有重复符号
# --start-group/--end-group: 解决预编译库之间的循环依赖
set(LIBS esp_audio_codec_lib esp_audio_simple_dec esp_new_jpeg esp_muxer esp_audio_effects)
target_link_libraries(${COMPONENT_TARGET} PRIVATE "-Wl,--allow-multiple-definition")
target_link_libraries(${COMPONENT_TARGET} PUBLIC "-Wl,--start-group" ${LIBS} "-Wl,--end-group")
