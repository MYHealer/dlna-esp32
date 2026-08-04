cmake_minimum_required(VERSION 3.5)
set(EXTRA_COMPONENT_DIRS ./components)
set(ADF_COMPONENTS_EXCLUDE
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
)
include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(project_name dlna)
project(${project_name})