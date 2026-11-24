idf_component_register(SRCS "app_vox_session.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_audio app_gemini_live app_secrets app_wake app_wifi esp_timer freertos heap
        mbedtls log)
