idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_secrets app_cogitador_eye app_cogitador_session app_wifi esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_bundle esp_mosaico_gsp_iris esp_hw_support
        nvs_flash log)
