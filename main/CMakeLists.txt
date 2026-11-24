idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_secrets app_vox_eye app_vox_session app_wifi esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_bundle esp_mosaico_gsp_iris esp_hw_support
        nvs_flash log)
