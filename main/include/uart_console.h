#ifndef UART_CONSOLE_H
#define UART_CONSOLE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configura el periférico UART0 (115200 8N1) y lanza la tarea FreeRTOS
 *        que escucha comandos seriales en segundo plano.
 * @return ESP_OK si el driver y la tarea se iniciaron correctamente, o código de error.
 */
esp_err_t uart_console_iniciar(void);

/**
 * @brief Detiene la tarea de monitoreo UART y desinstala el driver serie.
 */
void uart_console_detener(void);

#ifdef __cplusplus
}
#endif

#endif // UART_CONSOLE_H
