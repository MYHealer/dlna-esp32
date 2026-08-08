# Set the supported library variable for different chips

set(esp_h264_name "")

IF (CONFIG_IDF_TARGET STREQUAL "esp32s3")
set(esp_h264_name "esp_h264")
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./esp_h264/include)
ENDIF ()


# Add the source code and header file
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./esp_audio/include
                            ./esp_codec/include/codec
                            ./esp_codec/include/processing
                            ./esp_new_jpeg/include
                            ./media_lib_sal/include
                            ./media_lib_sal/include/port
                            ./esp_muxer/include
                            ./esp_audio_effects/include
                            ./esp_audio_codec/include
                            ./esp_audio_codec/include/simple_dec
                            ./esp_audio_codec/include/encoder
                            ./esp_audio_codec/include/encoder/impl
                            ./esp_audio_codec/include/decoder
                            ./esp_audio_codec/include/decoder/impl
                            ./esp_muxer/include
                            ./esp_muxer/include/impl)

if (CONFIG_MEDIA_PROTOCOL_LIB_ENABLE)
    list(APPEND COMPONENT_ADD_INCLUDEDIRS  ./esp_media_protocols/include)
endif()

list(APPEND COMPONENT_SRCS ./media_lib_sal/media_lib_common.c
                    ./media_lib_sal/media_lib_os.c
                    ./media_lib_sal/mem_trace/media_lib_mem_trace.c
                    ./media_lib_sal/mem_trace/media_lib_mem_his.c
                    ./media_lib_sal/port/media_lib_os_freertos.c
                    ./media_lib_sal/media_lib_adapter.c
                    # esp_audio_codec 注册源文件（嵌套组件不被 IDF 发现，手工加入）
                    # audio_encoder_reg.c 不需要（CONFIG_GMF_AUDIO_CODEC_INIT_ENCODER=n）
                    # 且 sp_alac_enc.h 缺少 stdbool.h 导致编译失败
                    ./esp_audio_codec/src/audio_decoder_reg.c
                    ./esp_audio_codec/src/simple_decoder_reg.c)

if (CONFIG_MEDIA_PROTOCOL_LIB_ENABLE)
list(APPEND COMPONENT_SRCS ./media_lib_sal/media_lib_crypt.c  ./media_lib_sal/port/media_lib_crypt_default.c
                           ./media_lib_sal/media_lib_socket.c ./media_lib_sal/port/media_lib_socket_default.c
                           ./media_lib_sal/media_lib_tls.c  ./media_lib_sal/port/media_lib_tls_default.c
                           ./media_lib_sal/media_lib_netif.c  ./media_lib_sal/port/media_lib_netif_default.c)
endif()


set(COMPONENT_REQUIRES nghttp esp-tls esp_netif freertos)

register_component()

# GCC 16+ 对 ADF 库代码的警告更严格，降级为非 error
target_compile_options(${COMPONENT_TARGET} PRIVATE
    -Wno-calloc-transposed-args
    -Wno-error=absolute-value
)


message(STATUS "Chip is ${CONFIG_IDF_TARGET}, ${CMAKE_CURRENT_SOURCE_DIR}")

set(TARGET_LIB_PATH "-L ${CMAKE_CURRENT_SOURCE_DIR}/esp_audio/lib/${CONFIG_IDF_TARGET}"
                    "-L ${CMAKE_CURRENT_SOURCE_DIR}/esp_codec/lib/${CONFIG_IDF_TARGET}"
                    "-L ${CMAKE_CURRENT_SOURCE_DIR}/esp_new_jpeg/lib/${CONFIG_IDF_TARGET}")

if (CONFIG_MEDIA_PROTOCOL_LIB_ENABLE)
    list(APPEND TARGET_LIB_PATH  "-L ${CMAKE_CURRENT_SOURCE_DIR}/esp_media_protocols/lib/${CONFIG_IDF_TARGET}")
endif()

target_link_libraries(${COMPONENT_TARGET} PRIVATE "${TARGET_LIB_PATH}")


# GREATER_EQUAL is only available on CMake +3.7.
# To be totally compatible with older versions, let's use GREATER instead
IF (IDF_VERSION_MAJOR GREATER 3)

    add_prebuilt_library(esp_audio "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio/lib/${CONFIG_IDF_TARGET}/libesp_audio.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_codec "${CMAKE_CURRENT_SOURCE_DIR}/esp_codec/lib/${CONFIG_IDF_TARGET}/libesp_codec.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_processing "${CMAKE_CURRENT_SOURCE_DIR}/esp_codec/lib/${CONFIG_IDF_TARGET}/libesp_processing.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_new_jpeg "${CMAKE_CURRENT_SOURCE_DIR}/esp_new_jpeg/lib/${CONFIG_IDF_TARGET}/libesp_new_jpeg.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_media_protocols "${CMAKE_CURRENT_SOURCE_DIR}/esp_media_protocols/lib/${CONFIG_IDF_TARGET}/libesp_media_protocols.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_h264 "${CMAKE_CURRENT_SOURCE_DIR}/${esp_h264_name}/lib/${CONFIG_IDF_TARGET}/lib${esp_h264_name}.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_muxer "${CMAKE_CURRENT_SOURCE_DIR}/esp_muxer/lib/${CONFIG_IDF_TARGET}/libesp_muxer.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_audio_effects "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_effects/lib/${CONFIG_IDF_TARGET}/libesp_audio_effects.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_audio_simple_dec "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_codec/lib/${CONFIG_IDF_TARGET}/libesp_audio_simple_dec.a"
                 PRIV_REQUIRES esp-adf-libs)
    add_prebuilt_library(esp_audio_codec_lib "${CMAKE_CURRENT_SOURCE_DIR}/esp_audio_codec/lib/${CONFIG_IDF_TARGET}/libesp_audio_codec.a"
                 PRIV_REQUIRES esp-adf-libs)
ENDIF (IDF_VERSION_MAJOR GREATER 3)

set(TARGET_LIB_NAME esp_processing esp_audio esp_codec esp_new_jpeg esp_muxer esp_audio_effects esp_audio_simple_dec esp_audio_codec_lib ${esp_h264_name})
if (CONFIG_MEDIA_PROTOCOL_LIB_ENABLE)
    list(APPEND TARGET_LIB_NAME esp_media_protocols)
endif()

# PUBLIC 让依赖 esp-adf-libs 的组件（如 gmf_audio）能链接到预编译库
# --allow-multiple-definition: 旧 libesp_codec.a 和新 libesp_audio_codec.a 都包含 FLAC 等解码器
target_link_libraries(${COMPONENT_TARGET} PRIVATE "-Wl,--allow-multiple-definition")
target_link_libraries(${COMPONENT_TARGET} PUBLIC "-Wl,--start-group" ${TARGET_LIB_NAME} "-Wl,--end-group")

