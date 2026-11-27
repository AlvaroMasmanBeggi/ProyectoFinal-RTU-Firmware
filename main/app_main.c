#include <stdlib.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "driver/i2c_types.h"
#include "esp_log.h"
#include "app_config.h"
#include "hal/gpio_types.h"
#include "wifi_manager.h"
#include "fota_service.h"
#include "uart_console.h"
#include "gpio_defs.h"
#include "irq_manager.h"

#include "BQ25628.h"
#include "bq27441.h"
#include "DS3231M.h"
#include "gpio_defs.h"



static const char *TAG = "RTU_MAIN";

/* =========================================================================
 * Declaración de Mutex / Semáforos compartidos
 * ========================================================================= */
/* Protege el acceso concurrente al bus I2C1 entre distintas tareas FreeRTOS */
SemaphoreHandle_t I2C1_Mutex = NULL;

/* =========================================================================
 * Declaración y configuración del Bus Maestro I2C1 (bus interno de la RTU)
 * ========================================================================= */
i2c_master_bus_handle_t I2C1_Handler;
i2c_master_bus_config_t I2C1_bus_cfg = {
    .clk_source = I2C_CLK_SRC_DEFAULT,          /* Fuente de reloj por defecto del ESP32-S3 */
    .i2c_port = I2C1_MASTER_PORT,               /* Puerto hardware I2C asignado */
    .scl_io_num = I2C1_SCL_PIN,                 /* Pin GPIO para línea de reloj SCL */
    .sda_io_num = I2C1_SDA_PIN,                 /* Pin GPIO para línea de datos SDA */
    .glitch_ignore_cnt = 7,                     /* Filtro digital contra ruido/glitches en el bus */
    .flags.enable_internal_pullup = false,      /* Deshabilitado: la placa ya tiene pull-ups externos a +3V3 */
};

// Declaracion de mutex/semaforos
SemaphoreHandle_t I2C1_Mutex = NULL;

// Declaracion de dispositivos
i2c_master_dev_handle_t BQ25628_dev = NULL;
i2c_master_dev_handle_t BQ27441_bus_handler = NULL;
BQ27441_ctx_t BQ27441_driver_dev;

// Estructuras de configuracion
bq25628_config_t charger_config;
BQ27441_config_t fuel_gauge_config;

// Estructuras de estado del sistema
battery_info_t battery_info;
bq25628_adc_data_t charger_info;

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

    ESP_ERROR_CHECK(bq25628_init(I2C1_Handler, &charger_config));
    ESP_ERROR_CHECK(BQ27441_initialization (I2C1_Handler, &BQ27441_driver_dev, &fuel_gauge_config));

    // Inicializar manejador de interrupciones y asignar GPIOs
    ESP_ERROR_CHECK(irq_manager_init());
    ESP_ERROR_CHECK(irq_manager_register_gpio(CHARGER_INT_PIN, GPIO_INTR_NEGEDGE, GPIO_FLOATING, IRQ_SRC_CHARGER_INT));
    ESP_ERROR_CHECK(irq_manager_register_gpio(FUELG_INT_PIN, GPIO_INTR_NEGEDGE, GPIO_FLOATING, IRQ_SRC_FUEL_GAUGE_INT));
    /* 1. Creación de mutex/semáforos antes de habilitar periféricos */
    I2C1_Mutex = xSemaphoreCreateMutex();
    configASSERT(I2C1_Mutex != NULL);   /* Detiene la ejecución si no hubo RAM para crear el Mutex */

    /* 2. Inicializar bus I2C1 maestro y registrar los dispositivos asociados */
    ESP_ERROR_CHECK(i2c_new_master_bus(&I2C1_bus_cfg, &I2C1_Handler));
    
    ESP_ERROR_CHECK(ds3231m_init(I2C1_Handler));

    /* 3. Configurar parámetros operativos del RTC DS3231M */
    ds3231m_config_t rtc_cfg = {
        .enable_osc_on_vbat = true,     /* Mantiene el reloj corriendo con la pila de respaldo en cortes */
        .enable_32khz_out   = false,    /* Deshabilita salida 32.768 kHz para minimizar consumo en batería */
        .int_pin_alarm_mode = true,     /* Usa pin INT/SQW como salida de interrupción por alarma */
        .enable_sqw_on_vbat = false,    /* No genera onda cuadrada cuando opera respaldado por batería */
        .enable_alarm1_int  = false,    /* Interrupción de Alarma 1 apagada en el arranque */
        .enable_alarm2_int  = false,    /* Interrupción de Alarma 2 apagada en el arranque */
    };
    ESP_ERROR_CHECK(ds3231m_config(&rtc_cfg));

    /* 4. Verificar si el RTC perdió alimentación (bandera OSF) para saber si la hora es válida */
    bool rtc_osc_stopped = false;
    ESP_ERROR_CHECK(ds3231m_is_oscillator_stopped(&rtc_osc_stopped));
    if (rtc_osc_stopped) {
        ESP_LOGW(TAG, "RTC sin hora valida (OSF=1). Pendiente de sincronizacion SNTP.");
    } else {
        ds3231m_time_t fecha_actual;
        if (ds3231m_get_time(&fecha_actual) == ESP_OK) {
            ESP_LOGI(TAG, "Fecha/Hora RTC: %02u/%02u/%04u %02u:%02u:%02u",
                     fecha_actual.date, fecha_actual.month, fecha_actual.year,
                     fecha_actual.hours, fecha_actual.minutes, fecha_actual.seconds);
        }
    }


    ESP_LOGI(TAG, "=== BAIGORRIA/MASMAN ===\n");

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

            // La imagen se confirma cuando el cliente TLS recibe una respuesta válida del servidor.
            fota_service_iniciar();
        } else {
            ESP_LOGW(TAG, "Fallo crítico al conectar a Wi-Fi. Activando portal cautivo AP...");
            wifi_manager_iniciar_ap();
        }
    } else {
        ESP_LOGW(TAG, "Configuración no encontrada o inválida. Activando portal cautivo AP...");
        wifi_manager_iniciar_ap();
    }

    // 4. Iniciar monitor UART en background (escucha comando 'config')
    ESP_ERROR_CHECK(uart_console_iniciar());

    ESP_LOGI(TAG, "Sistema inicializado en su totalidad.");
}
