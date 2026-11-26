idf_component_register(SRCS "app_rol_web.c" "rol_players.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server esp_netif esp_hw_support cjson freertos heap log
    EMBED_TXTFILES "rol_page.html")
