idf_component_register(SRCS "app_secrets.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_iris nvs_flash mbedtls log)
