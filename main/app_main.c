#include <stdio.h>
#include "esp_log.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "fota_service.h"
#include "uart_console.h"

static const char *TAG = "RTU_MAIN";

void app_main(void){
    ESP_LOGI(TAG, "=== Iniciando RTU AgroConnect ===");

    // 1. Inicializar la memoria no volátil (NVS)
    ESP_ERROR_CHECK(app_config_init_nvs());

    // 2. Inicializar la pila TCP/IP, eventos y controlador Wi-Fi
    ESP_ERROR_CHECK(wifi_manager_init());

    // 3. Evaluar credenciales almacenadas e intentar enlace
    app_config_t config;
    bool config_presente = app_config_cargar(&config);

    if (config_presente && app_config_validar(&config)) {
        ESP_LOGI(TAG, "Configuración válida en memoria. Conectando a red Wi-Fi: %s", config.ssid);

        if (wifi_manager_conectar_sta(&config)) {
            ESP_LOGI(TAG, "¡Conexión Wi-Fi establecida con éxito!");

            // Valida el firmware ante el bootloader para evitar rollback
            fota_confirmar_app_si_valida();

            // Inicia la tarea de telemetría y cliente FOTA TLS
            fota_service_iniciar();
        } else {
            ESP_LOGW(TAG, "Fallo crítico al conectar a Wi-Fi. Activando portal cautivo AP...");
            wifi_manager_iniciar_ap();
            fota_confirmar_app_si_valida();
        }
    } else {
        ESP_LOGW(TAG, "Configuración no encontrada o inválida. Activando portal cautivo AP...");
        wifi_manager_iniciar_ap();
        fota_confirmar_app_si_valida();
    }

    // 4. Iniciar monitor UART en background (escucha comando 'config')
    ESP_ERROR_CHECK(uart_console_iniciar());

    ESP_LOGI(TAG, "Sistema inicializado en su totalidad.");
}
