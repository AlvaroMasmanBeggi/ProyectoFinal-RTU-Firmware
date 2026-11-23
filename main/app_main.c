#include <stdio.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "esp_log.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "fota_service.h"
#include "uart_console.h"
#include "gpio_defs.h"

#include "BQ25628.h"

static const char *TAG = "RTU_MAIN";

// Declaracion de mutex/semaforos
SemaphoreHandle_t I2C1_Mutex = NULL;

// Declaracion de dispositivos
i2c_master_dev_handle_t BQ25628_dev = NULL;

// Declaracion de manejadores
i2c_master_bus_handle_t I2C1_Handler;
i2c_master_bus_config_t I2C1_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C1_MASTER_PORT,
        .scl_io_num = I2C1_SCL_PIN,
        .sda_io_num = I2C1_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

void app_main(void){
	ESP_LOGI(TAG, "=== Iniciando RTU AgroConnect ===\n");

	// Creacion de mutex/semaforos
	I2C1_Mutex = xSemaphoreCreateMutex();

	// Inicializar bus I2C1 maestro y periféricos asociados
    ESP_ERROR_CHECK(i2c_new_master_bus(&I2C1_bus_cfg, &I2C1_Handler));
    ESP_ERROR_CHECK(bq25628_init(I2C1_Handler));

    ESP_LOGI(TAG, "=== ESTE FIRMWARE LO SUBI CON FOTA POR SEGUNDA VEZ ===\n");

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
