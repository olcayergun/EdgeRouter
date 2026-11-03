# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/*.S"
)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
                                    "${CMAKE_CURRENT_LIST_DIR}/../include"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash)