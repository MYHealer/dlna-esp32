idf_component_register(
    SRCS "wifi_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_netif lwip
)
