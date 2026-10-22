idf_component_register(SRCS "main.c" "odid_encode.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_timer nvs_flash driver)
