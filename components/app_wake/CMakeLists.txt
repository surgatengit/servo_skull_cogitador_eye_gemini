idf_component_register(SRCS "app_wake.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp-sr freertos heap log)
