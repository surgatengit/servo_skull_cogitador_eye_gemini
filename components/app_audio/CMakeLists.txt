idf_component_register(SRCS "app_audio.c" "app_audio_fir.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp-mosaico-bsp esp_timer freertos heap log)
