idf_component_register(SRCS "app_photos.c" "photos_ring.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_partition esp_rom log)
