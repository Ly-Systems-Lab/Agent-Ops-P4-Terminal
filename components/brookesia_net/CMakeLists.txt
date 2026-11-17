set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS "include"
    # nvs_flash：读「设置 → WLAN」那个开关（storage/wifi_en）
    # esp_timer：连续失败后 15 秒一次的后备重连
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash
)
