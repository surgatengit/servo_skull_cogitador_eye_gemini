idf_component_register(SRCS "app_camera.c" "camera_geometry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES mosaico_module_camera freertos esp_driver_ppa esp_driver_jpeg esp_mm esp_timer log)
