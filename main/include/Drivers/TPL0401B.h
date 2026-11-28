#ifndef TPL0401B_H
#define TPL0401B_H

#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/i2c_master.h>

#define USE_MUTEX 1							// Colocar en 1 para habilitar mutex en lecturas/escrituras I2C
#define MUTEX_TIMEOUT 100					// Tiempo en milisegundos que debe pasar para ignorar la toma del mutex
#define I2C_TIMEOUT 100						// Tiempo en milisegundos que debe pasar para indicar fallo en transaccion I2C

#define TPL0401B_I2C_ADDR       0x3E    	// Dirección I2C de 7 bits (TPL0401B)
#define TPL0401_REG_WIPER       0x00    	// Registro interno (Wiper Position)

extern SemaphoreHandle_t I2C1_Mutex;
extern i2c_master_dev_handle_t TPL0401_dev;

/**
 * @brief Establece el nivel de contraste del display
 *
 * @param[in] contrast_pct Porcentaje de contraste deseado (0 a 100).
 *
 * @return
 *      - ESP_OK: Posición actualizada correctamente.
 *      - ESP_ERR_TIMEOUT: No se pudo adquirir el mutex del bus I2C dentro del tiempo límite.
 *      - Otros códigos de error propios de tpl0401_i2c_write() / i2c_master_transmit().
 */
esp_err_t tpl0401_set_step(uint8_t contrast_pct);

/**
 * @brief Lee la posición actual del cursor
 *
 * @param[out] contrast_pct Puntero a la variable donde se almacenará el porcentaje de
 *                          contraste actual (0 a 100).
 *
 * @return
 *      - ESP_OK: Lectura realizada correctamente.
 *      - ESP_ERR_INVALID_ARG: El puntero contrast_pct es NULL.
 *      - ESP_ERR_TIMEOUT: No se pudo adquirir el mutex del bus I2C dentro del tiempo límite.
 *      - Otros códigos de error propios de tpl0401_i2c_read() / i2c_master_transmit_receive().
 */
esp_err_t tpl0401_get_step(uint8_t *contrast_pct);

#endif
