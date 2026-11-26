#ifndef IRQ_MANAGER_H
#define IRQ_MANAGER_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

/**
 * @brief Máscara de bits para fuentes de interrupción de hardware (hasta 32 fuentes).
 */
typedef enum {
    IRQ_SRC_CHARGER_INT     	= (1U << 0),    // Cargador BQ25628
    IRQ_SRC_FUEL_GAUGE_INT      = (1U << 1),    // Fuel Gauge BQ27441
	IRQ_SRC_BUTTON_INT      	= (1U << 2),    // Teclado
} irq_source_t;

/**
 * @brief Instala el servicio global de ISR de GPIO de ESP-IDF y crea la tarea
 *        central encargada de despachar los eventos de hardware.
 *
 * @return ESP_OK si se inicializó correctamente, o código de error de ESP-IDF.
 */
esp_err_t irq_manager_init(void);

/**
 * @brief Registra un pin GPIO como fuente de interrupción asociada a un bit de irq_source_t.
 *
 * @param[in] gpio_num   Número de pin GPIO del ESP32-S3.
 * @param[in] intr_type  Tipo de flanco (ej. GPIO_INTR_NEGEDGE, GPIO_INTR_POSEDGE).
 * @param[in] pull_mode  Configuración de resistencia interna (ej. GPIO_PULLUP_ONLY, GPIO_FLOATING).
 * @param[in] source_bit Bit único de irq_source_t que se notificará al disparar el pin.
 *
 * @return ESP_OK si el pin quedó configurado y enlazado a la ISR genérica.
 */
esp_err_t irq_manager_register_gpio(gpio_num_t gpio_num, gpio_int_type_t intr_type, gpio_pull_mode_t pull_mode,irq_source_t source_bit);

#endif
