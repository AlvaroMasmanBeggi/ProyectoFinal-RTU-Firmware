idf_component_register(
    SRCS
        "app_main.c"
        "src/app_config.c"
        "src/wifi_manager.c"
        "src/web_portal.c"
        "src/fota_service.c"
        "src/uart_console.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp-tls
        app_update
        mbedtls
        driver
        esp_driver_uart
    # EMBED_TXTFILES
    #    "../certs/ota_server_ca.pem"  # Descomentar al activar FOTA_VERIFY_TLS_SERVER
)
