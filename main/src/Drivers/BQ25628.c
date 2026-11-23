#include "BQ25628.h"

esp_err_t bq25628_init(i2c_master_bus_handle_t bus_handle){
	if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;
    if (BQ25628_dev != NULL) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BQ25628_I2C_ADDR,
        .scl_speed_hz = BQ25628_I2C_SPEED_HZ,
    };

    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &BQ25628_dev);

}

esp_err_t bq25628_i2c_write(uint8_t reg_addr, const uint8_t *data, size_t len){
    if (data == NULL || len == 0 || (len + 1) > 64) return ESP_ERR_INVALID_ARG;

#if USE_MUTEX
    if (I2C1_Mutex == NULL || xSemaphoreTake(I2C1_Mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT)) != pdTRUE) return ESP_ERR_TIMEOUT;
#endif

    uint8_t tx_buf[64];
    tx_buf[0] = reg_addr;
    memcpy(&tx_buf[1], data, len);

    esp_err_t ret = i2c_master_transmit(BQ25628_dev, tx_buf, len + 1, I2C_TIMEOUT);

#if USE_MUTEX
    xSemaphoreGive(I2C1_Mutex);
#endif

    return ret;
}

esp_err_t bq25628_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len){
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

#if USE_MUTEX
    if (I2C1_Mutex == NULL || xSemaphoreTake(I2C1_Mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT)) != pdTRUE) return ESP_ERR_TIMEOUT;
#endif

    esp_err_t ret = i2c_master_transmit_receive(BQ25628_dev, &reg_addr, 1, data, len, I2C_TIMEOUT);

#if USE_MUTEX
    xSemaphoreGive(I2C1_Mutex);
#endif

    return ret;
}
