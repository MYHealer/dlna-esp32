cmake_minimum_required(VERSION 3.5)
set(EXTRA_COMPONENT_DIRS ./components)

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXCLUDE_COMPONENTS
    adf_utils
    audio_recorder
    battery_service
    bluetooth_service
    clouds
    coredump_upload_service
    dueros_service
    esp_actions
    esp_coze
    input_key_service
    ota_service
    playlist
    tone_partition
    esp-sr
    wifi_service
    display_service
    esp_dispatcher
    esp_event_cast
    esp_codec_dev
    audio_hal
    audio_board
)

set(project_name dlna)
project(${project_name})

# IDF 6.2 兼容层
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    # 全局注入兼容头文件（GPIO/UART/I2S/WiFi 重命名）
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/components/idf6_compat/idf6_compat.h" _compat_h)
    add_compile_options(-include "${_compat_h}")
    # 包装头文件目录（esp_wpa2.h → esp_eap_client.h + 函数声明）
    # 不含 driver/ 子目录，rotary_encoder.c 已用条件编译处理
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/components/idf6_compat")
endif()
