idf_component_register(SRCS "app_vox_session.c" "vox_status_text.c" "vox_rol.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_audio app_camera app_gemini_live app_photos app_rol_web app_secrets app_wake app_wifi cjson esp_hw_support esp_timer freertos heap
        mbedtls log)
