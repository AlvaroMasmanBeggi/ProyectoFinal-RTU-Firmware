idf_component_register(SRCS "hello_world_main.c"
                       INCLUDE_DIRS ""
                       EMBED_TXTFILES "ota_server_ca.pem"
                       REQUIRES app_update esp-tls esp_wifi nvs_flash
                               esp_event esp_http_server esp_driver_uart
                               esp_partition mbedtls)
