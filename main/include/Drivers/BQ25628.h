#ifndef BQ25628_H
#define BQ25628_H

#include <stdlib.h>
#include <esp_err.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/i2c_master.h>

// Mapa de registros

#define BQ25628_CHARGE_ILIM			0x02
#define BQ25628_CHARGE_VLIM			0x04
#define BQ25628_INPUT_ILIM			0x06 		// No se usa ya que la corriente maxima de entrada se define con resistencia conectada a ILIM
#define BQ25628_INPUT_VLIM			0x08
#define BQ25628_VOTG_REG			0x0C
#define BQ25628_MIN_VSYS			0x0E
#define BQ25628_PRECHARGE_CTRL		0x10
#define BQ25628_TERM_CTRL			0x12
#define BQ25628_CHARGE_CTRL			0x14
#define BQ25628_CHARGE_TIM_CTRL		0x15
#define BQ25628_CHARGER_CTRL0		0x16
#define BQ25628_CHARGER_CTRL1		0x17
#define BQ25628_CHARGER_CTRL2		0x18
#define BQ25628_CHARGER_CTRL3		0x19
#define BQ25628_NTC_CTRL0			0x1A		// Se mantienen los valores POR
#define BQ25628_NTC_CTRL1			0x1B		// Se mantienen los valores POR
#define BQ25628_NTC_CTRL2			0x1C		// Se mantienen los valores POR

#define BQ25628_CHARGER_STAT0		0x1D
#define BQ25628_CHARGER_STAT1		0x1E
#define BQ25628_FAULT_STAT			0x1F

#define BQ25628_CHARGER_FLAG0		0x20
#define BQ25628_CHARGER_FLAG1		0x21
#define BQ25628_FAULT_FLAG			0x22

#define BQ25628_CHARGER_MASK0		0x23
#define BQ25628_CHARGER_MASK1		0x24		// Se mantienen los valores POR
#define BQ25628_FAULT_MASK			0x25

#define BQ25628_ADC_CTRL			0x26
#define BQ25628_ADC_FUNCTION		0x27		// Se mantienen los valores POR

#define BQ25628_ADC_IBUS			0x28
#define BQ25628_ADC_IBAT			0x2A
#define BQ25628_ADC_VBUS			0x2C
#define BQ25628_ADC_VPMID			0x2E
#define BQ25628_ADC_VBAT			0x30
#define BQ25628_ADC_VSYS			0x32
#define BQ25628_ADC_TS				0x34
#define BQ25628_ADC_TDIE			0x36

#define BQ25628_PARTINFO			0x38


// Definiciones varias

#define BQ25628_I2C_ADDR        0x6A	    // Dirección I2C de 7 bits
#define BQ25628_I2C_SPEED_HZ    400000  	// 400 kHz

#define USE_MUTEX 1							// Colocar en 1 para habilitar mutex en lecturas/escrituras I2C
#define MUTEX_TIMEOUT 100					// Tiempo en milisegundos que debe pasar para ignorar la toma del mutex
#define I2C_TIMEOUT 100						// Tiempo en milisegundos que debe pasar para indicar fallo en transaccion I2C

extern SemaphoreHandle_t I2C1_Mutex;
extern i2c_master_dev_handle_t BQ25628_dev;

// Estructura de configuracion

typedef struct {
	uint16_t charge_current_ma;     // Corriente de carga en mA (40 a 2000)
	uint16_t charge_voltage_mv;     // Voltaje límite de carga en mV (3500 a 4800)
	uint16_t input_voltage_mv;		// Voltaje minimo de entrada en mV (3800 a 16800)
	uint16_t min_sys_voltage_mv;	// Voltaje minimo de salida en mV (2560 a 3840)
	uint16_t precharge_current_ma;	// Corriente de pre-carga en mA (10 a 310)
	uint16_t term_current_ma;		// Corriente de terminacion en mA (5 a 310)

} bq25628_config_t;

// Definicion de funciones

/**
 * @brief Inicializa el driver
 *
 * @param[in] bus_handle Handle del bus I2C maestro.
 *
 * @return
 *      - ESP_OK: Dispositivo registrado y listo para operar.
 *      - ESP_ERR_INVALID_ARG: El handle del bus I2C es NULL.
 *      - ESP_ERR_INVALID_STATE: El driver ya había sido inicializado previamente.
 *      - Otros códigos de error propios de i2c_master_bus_add_device().
 */
esp_err_t bq25628_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Escribe uno o varios bytes en un registro específico
 *
 * @param[in] reg_addr Dirección del registro interno del BQ25628 donde comenzará la escritura.
 * @param[in] data     Puntero al buffer con los datos a transmitir.
 * @param[in] len      Cantidad de bytes a escribir (rango válido: 1 a 63).
 *
 * @return
 *      - ESP_OK: Escritura realizada con éxito.
 *      - ESP_ERR_INVALID_ARG: Puntero nulo, longitud igual a 0 o superior a 63 bytes.
 *      - ESP_ERR_TIMEOUT: No se pudo adquirir el mutex I2C dentro del tiempo límite
 *      - Otros códigos de error propios de i2c_master_transmit().
 */
esp_err_t bq25628_i2c_write(uint8_t reg_addr, const uint8_t *data, size_t len);

/**
 * @brief Lee uno o varios bytes consecutivos a partir de un registro determinado
 *
 * @param[in]  reg_addr Dirección del registro inicial a leer en el BQ25628.
 * @param[out] data     Puntero al buffer donde se almacenarán los bytes leídos.
 * @param[in]  len      Cantidad de bytes a leer (debe ser mayor a 0).
 *
 * @return
 *      - ESP_OK: Lectura realizada con éxito.
 *      - ESP_ERR_INVALID_ARG: Puntero nulo o longitud igual a 0.
 *      - ESP_ERR_TIMEOUT: No se pudo adquirir el mutex I2C dentro del tiempo límite.
 *      - Otros códigos de error propios de i2c_master_transmit_receive().
 */
esp_err_t bq25628_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len);


/**
 * @brief Configura los parámetros operativos del cargador
 *
 * @param[in] config Puntero a la estructura con los parámetros de configuración
 *
 * @return
 *      - ESP_OK: Todos los registros fueron configurados correctamente.
 *      - ESP_ERR_INVALID_ARG: El puntero config es NULL.
 *      - ESP_ERR_TIMEOUT: No se pudo adquirir el mutex del bus I2C dentro del tiempo límite.
 *      - Otros códigos de error propios de bq25628_i2c_write() / i2c_master_transmit().
 */
esp_err_t bq25628_config(bq25628_config_t *config);








#endif
