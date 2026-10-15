#include "uart_console.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "wifi_manager.h"
#include "fota_service.h"

static const char *TAG = "UART_CONSOLE";

#define UART_CONSOLE_PORT        UART_NUM_0
#define UART_CONSOLE_BAUD_RATE   115200
#define UART_RX_BUF_SIZE         512
#define UART_LINE_MAX_LEN        64

static TaskHandle_t s_uart_task_handle = NULL;
static bool s_driver_instalado = false;

static void uart_monitor_task(void *pvParameters){
    char line_buffer[UART_LINE_MAX_LEN];
    int rx_index = 0;

    ESP_LOGI(TAG, "Monitor de consola UART iniciado en UART%d a %d bps.",
             UART_CONSOLE_PORT, UART_CONSOLE_BAUD_RATE);

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(UART_CONSOLE_PORT, &byte, 1, portMAX_DELAY);

        if (len > 0) {
            // Fin de línea detectado (Enter: CR o LF)
            if (byte == '\r' || byte == '\n') {
                if (rx_index > 0) {
                    line_buffer[rx_index] = '\0';

                    if (strcmp(line_buffer, "config") == 0) {
                        ESP_LOGW(TAG, "¡Comando 'config' recibido! Deteniendo servicios para iniciar Portal AP...");

                        // Detiene el cliente TLS si estaba corriendo y levanta el modo AP
                        fota_service_detener();
                        wifi_manager_iniciar_ap();
                    } else {
                        ESP_LOGI(TAG, "Comando no reconocido: '%s'", line_buffer);
                    }

                    rx_index = 0;
                }
            } else {
                // Almacenar byte recibido evitando desbordamientos de buffer
                if (rx_index < (UART_LINE_MAX_LEN - 1)) {
                    line_buffer[rx_index++] = (char)byte;
                } else {
                    // Si el comando excede el tamaño máximo, se descarta la línea
                    rx_index = 0;
                }
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t uart_console_iniciar(void){
    if (s_uart_task_handle != NULL) {
        ESP_LOGW(TAG, "La consola UART ya está en ejecución.");
        return ESP_OK;
    }

    if (!s_driver_instalado) {
        uart_config_t uart_config = {
            .baud_rate  = UART_CONSOLE_BAUD_RATE,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        esp_err_t err = uart_driver_install(UART_CONSOLE_PORT, UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error instalando driver UART: %s", esp_err_to_name(err));
            return err;
        }

        err = uart_param_config(UART_CONSOLE_PORT, &uart_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error configurando parámetros UART: %s", esp_err_to_name(err));
            return err;
        }

        err = uart_set_pin(UART_CONSOLE_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error asignando pines UART: %s", esp_err_to_name(err));
            return err;
        }

        s_driver_instalado = true;
    }

    BaseType_t res = xTaskCreate(uart_monitor_task, "uart_monitor_task", 4096, NULL, 10, &s_uart_task_handle);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de consola UART.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void uart_console_detener(void){
    if (s_uart_task_handle != NULL) {
        vTaskDelete(s_uart_task_handle);
        s_uart_task_handle = NULL;
    }

    if (s_driver_instalado) {
        uart_driver_delete(UART_CONSOLE_PORT);
        s_driver_instalado = false;
        ESP_LOGI(TAG, "Driver UART desinstalado.");
    }
}
