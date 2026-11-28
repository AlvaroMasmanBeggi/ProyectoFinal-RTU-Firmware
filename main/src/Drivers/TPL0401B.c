#include "TPL0401B.h"

/**
 * @brief Escribe uno o varios bytes en un registro específico del BQ27441.
 *
 * @param[in] dev_addr Dirección I2C del dispositivo (gestionada internamente por BQ27441_dev).
 * @param[in] reg_addr Dirección del registro interno del BQ27441 donde comenzará la escritura.
 * @param[in] data     Puntero al buffer con los datos a transmitir.
 * @param[in] len      Cantidad de bytes a escribir (rango válido: 1 a 63).
 *
 * @return
 *      -  1: Escritura realizada con éxito.
 *      -  0: Puntero nulo, longitud inválida, fallo al adquirir el mutex o error en i2c_master_transmit().
 */
static esp_err_t tpl0401_i2c_write(uint8_t reg_addr, const uint8_t *data, size_t len){
    if (TPL0401_dev == NULL || data == NULL || len == 0 || (len + 1) > 8) return ESP_ERR_INVALID_ARG;

    #if USE_MUTEX
    	if (I2C1_Mutex == NULL || xSemaphoreTake(I2C1_Mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT)) != pdTRUE) return ESP_ERR_TIMEOUT;
    #endif

    uint8_t tx_buf[8];
    tx_buf[0] = reg_addr;
    memcpy(&tx_buf[1], data, len);

    esp_err_t ret = i2c_master_transmit(TPL0401_dev, tx_buf, len + 1, I2C_TIMEOUT);

    #if USE_MUTEX
    	xSemaphoreGive(I2C1_Mutex);
     #endif

    return ret;
}


/**
 * @brief Lee uno o varios bytes consecutivos a partir de un registro determinado del BQ27441.
 *
 * @param[in]  dev_addr Dirección I2C del dispositivo (gestionada internamente por BQ27441_dev).
 * @param[in]  reg_addr Dirección del registro inicial a leer en el BQ27441.
 * @param[out] data     Puntero al buffer donde se almacenarán los bytes leídos.
 * @param[in]  len      Cantidad de bytes a leer (debe ser mayor a 0).
 *
 * @return
 *      -  1: Lectura realizada con éxito.
 *      -  0: Puntero nulo, longitud igual a 0, fallo al adquirir el mutex o error en i2c_master_transmit_receive().
 */
static esp_err_t tpl0401_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len){
    if (TPL0401_dev == NULL || data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    #if USE_MUTEX
    	if (I2C1_Mutex == NULL || xSemaphoreTake(I2C1_Mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT)) != pdTRUE) return ESP_ERR_TIMEOUT;
     #endif

    esp_err_t ret = i2c_master_transmit_receive(TPL0401_dev, &reg_addr, 1, data, len, I2C_TIMEOUT);

    #if USE_MUTEX
    	xSemaphoreGive(I2C1_Mutex);
     #endif

    return ret;
}

esp_err_t tpl0401_set_step(uint8_t contrast_pct){
    // Saturación (Clamped High)
    if (contrast_pct > 100U) contrast_pct = 100U;

    // Conversión porcentual (0..100 %) a paso de 7 bits (0..127) con redondeo
    uint8_t wiper_val = (uint8_t)(((uint16_t)contrast_pct * 127U + 50U) / 100U) & 0x7FU;

    return tpl0401_i2c_write(TPL0401_REG_WIPER, &wiper_val, 1);
}

esp_err_t tpl0401_get_step(uint8_t *contrast_pct){
    if (contrast_pct == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t raw_val = 0U;
    esp_err_t ret = tpl0401_i2c_read(TPL0401_REG_WIPER, &raw_val, 1);
    if (ret != ESP_OK) return ret;

    // Enmascarado a 7 bits y conversión inversa (0..127) a porcentaje (0..100 %)
    uint8_t step = raw_val & 0x7FU;
    *contrast_pct = (uint8_t)(((uint16_t)step * 100U + 63U) / 127U);

    return ESP_OK;
}
