idf_component_register(SRCS "app_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_config esp_wifi esp_netif esp_event esp_timer esp_hw_support nvs_flash mbedtls log)
