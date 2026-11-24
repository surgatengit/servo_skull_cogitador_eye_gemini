idf_component_register(SRCS "app_gemini_live.c" "gl_grounding.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_websocket_client cjson esp-tls mbedtls freertos heap log)
