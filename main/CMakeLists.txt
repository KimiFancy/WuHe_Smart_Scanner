idf_component_register(SRCS "wuhe_backup.c" "wuhe_storage.c" "wuhe_cloud.c" "internet_mgr.c" "screen_display.c" "hid_host_example.c" "MyFont.c"
                        PRIV_REQUIRES esp_timer esp_driver_gpio esp_wifi nvs_flash esp_http_client esp-tls esp_event
                       INCLUDE_DIRS "."
                       )
