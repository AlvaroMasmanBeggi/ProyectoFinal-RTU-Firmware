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

esp_err_t bq25628_config(bq25628_config_t *config){
	if (config == NULL) return ESP_ERR_INVALID_ARG;

	uint8_t reg_bytes[2];\
	esp_err_t ret;

	/* --- Charge Current Limit (ICHG) --- */

	uint16_t ichg_ma = config->charge_current_ma;

	// Saturación (Clamped Low / Clamped High)
    if (ichg_ma < 40U) ichg_ma = 40U;
    else if (ichg_ma > 2000U) ichg_ma = 2000U;

    // Conversión decimal (mA) a código de 6 bits y alineación de bits
    uint16_t ichg_code = ichg_ma / 40U;
    uint16_t reg_val   = (ichg_code << 5U) & 0x07E0U;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = reg_val & 0xFFU;
    reg_bytes[1] = (reg_val >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_CHARGE_ILIM, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Charge Voltage Limit (VREG) --- */

    uint16_t vreg_mv = config->charge_voltage_mv;

    // Saturación (Clamped Low / Clamped High)
    if (vreg_mv < 3500U) vreg_mv = 3500U;
    else if (vreg_mv > 4800U) vreg_mv = 4800U;

    // Conversión decimal (mV) a código y alineación de bits
    uint16_t vreg_code = vreg_mv / 10U;
    uint16_t vreg_reg = (vreg_code << 3) & 0x0FF8U;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = vreg_reg & 0xFFU;
    reg_bytes[1] = (vreg_reg >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_CHARGE_VLIM, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Input Voltage Limit (VINDPM) --- */

    uint16_t vindpm_mv = config->input_voltage_mv;

    // Saturación (Clamped Low / Clamped High)
    if (vindpm_mv < 3800U) vindpm_mv = 3800U;
    else if (vindpm_mv > 16800U) vindpm_mv = 16800U;

    // Conversión decimal (mV) a código y alineación de bits
    uint16_t vindpm_code = vindpm_mv / 40U;
    uint16_t vindpm_reg = (vindpm_code << 5U) & 0x3FE0U;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = vindpm_reg & 0xFFU;
    reg_bytes[1] = (vindpm_reg >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_INPUT_VLIM, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Minimal System Voltage (VSYSMIN) --- */

    uint16_t vsysmin_mv = config->min_sys_voltage_mv;

    // Saturación (Clamped Low / Clamped High)
    if (vsysmin_mv < 2560U) vsysmin_mv = 2560U;
    else if (vsysmin_mv > 3840U) vsysmin_mv = 3840U;

    // Conversión decimal (mV) a código y alineación de bits
    uint16_t vsysmin_code = vsysmin_mv / 80U;
    uint16_t vsysmin_reg = (vsysmin_code << 6U) & 0x0FC0U;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = vsysmin_reg & 0xFFU;
    reg_bytes[1] = (vsysmin_reg >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_MIN_VSYS, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Pre-charge Control (IPRECHG) --- */

    uint16_t iprechg_ma = config->precharge_current_ma;

    // Saturación (Clamped Low / Clamped High)
    if (iprechg_ma < 10U) iprechg_ma = 10U;
    else if (iprechg_ma > 310U) iprechg_ma = 310U;

    // Conversión decimal (mA) a código y alineación de bits
    uint16_t iprechg_code = iprechg_ma / 10U;
    uint16_t iprechg_reg = (iprechg_code << 3U) & 0x00F8U;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = iprechg_reg & 0xFFU;
    reg_bytes[1] = (iprechg_reg >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_PRECHARGE_CTRL, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Termination Control (ITERM) --- */

    uint16_t iterm_ma = config->term_current_ma;

    // Saturación (Clamped Low / Clamped High)
    if (iterm_ma < 5U) iterm_ma = 5U;
    else if (iterm_ma > 310U) iterm_ma = 310U;

    // Conversión decimal (mA) a código y alineación de bits
    uint16_t iterm_code = iterm_ma / 5U;
    uint16_t iterm_reg = (iterm_code << 2U) & 0x00FCU;

    // Empaquetado en 2 bytes Little-Endian
    reg_bytes[0] = iterm_reg & 0xFFU;
    reg_bytes[1] = (iterm_reg >> 8) & 0xFFU;

    ret = bq25628_i2c_write(BQ25628_TERM_CTRL, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    /* --- Charge Control --- */

    // Q1_FULLON=0, Q4_FULLON=1, ITRICKLE=1 (40mA), TOPOFF_TMR=00, EN_TERM=1, VINDPM_BAT_TRACK=0, VRECHG=0
    reg_bytes[0] = (0U << 7U) | (1U << 6U) | (1U << 5U) | (0U << 4U) | (0U << 3U) | (1U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGE_CTRL, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charge Timer Control --- */

    // DIS_STAT=0, EN_AUTO_INDET=0, FORCE_INDET=0, EN_DCP_BIAS=0, TMR2X_EN=1, EN_SAFETY_TMRS=1, PRECHG_TMR=0 (2h), CHG_TMR=0 (12h)
    reg_bytes[0] = (0U << 7U) | (0U << 6U) | (0U << 5U) | (0U << 4U) | (1U << 3U) | (1U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGE_TIM_CTRL, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charger Control 0 --- */

    // EN_AUTO_IBATDIS=1, FORCE_IBATDIS=0, EN_CHG=1, EN_HIZ=0, FORCE_PMID_DIS=0, WD_RST=0, WATCHDOG=00 (deshabilitado)
    reg_bytes[0] = (1U << 7U) | (0U << 6U) | (1U << 5U) | (0U << 4U) | (0U << 3U) | (0U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGER_CTRL0, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charger Control 1 --- */

    // REG_RST=0, TREG=1 (120°C), SET_CONV_FREQ=00 (1.5MHz), SET_CONV_STRN=11, RESERVED=0, VBUS_OVP=1 (18.5V)
    reg_bytes[0] = (0U << 7U) | (1U << 6U) | (0U << 5U) | (0U << 4U) | (1U << 3U) | (1U << 2U) | (0U << 1U) | (1U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGER_CTRL1, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charger Control 2 --- */

    // EN_BYPASS_OTG=0, EN_OTG=0, PFM_OTG_DIS=0, PFM_FWD_DIS=0, BATFET_CTRL_WVBUS=0, BATFET_DLY=1 (12.5s), BATFET_CTRL=00 (Normal)
    reg_bytes[0] = (0U << 7U) | (0U << 6U) | (0U << 5U) | (0U << 4U) | (0U << 3U) | (1U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGER_CTRL2, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charger Control 3 --- */

    // IBAT_PK=11 (12A, POR), VBAT_UVLO=0, VBAT_OTG_MIN=0, RESERVED=0, EN_EXTILIM=1, CHG_RATE=00 (1C)
    reg_bytes[0] = (1U << 7U) | (1U << 6U) | (0U << 5U) | (0U << 4U) | (0U << 3U) | (1U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGER_CTRL3, reg_bytes, 1);
    if (ret != ESP_OK) return ret;



    return ESP_OK;
}
