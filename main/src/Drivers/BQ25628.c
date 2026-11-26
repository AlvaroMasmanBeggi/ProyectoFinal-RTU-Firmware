#include "BQ25628.h"
#include "freertos/idf_additions.h"
#include <stdint.h>

SemaphoreHandle_t bq25628_adc_sem = NULL;

esp_err_t bq25628_init(i2c_master_bus_handle_t bus_handle){
	if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;
    if (BQ25628_dev != NULL) return ESP_ERR_INVALID_STATE;

    if (bq25628_adc_sem == NULL) {
        bq25628_adc_sem = xSemaphoreCreateBinary();
        if (bq25628_adc_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

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

    /* --- Charger Mask 0 --- */

    // RESERVED=0, ADC_DONE_MASK=0, TREG_MASK=1, VSYS_MASK=1, IINDPM_MASK=1, VINDPM_MASK=1, SAFETY_TMR_MASK=1, WD_MASK=1
    reg_bytes[0] = (0U << 7U) | (0U << 6U) | (1U << 5U) | (1U << 4U) | (1U << 3U) | (1U << 2U) | (1U << 1U) | (1U << 0U);

    ret = bq25628_i2c_write(BQ25628_CHARGER_MASK0, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- FAULT Mask 0 --- */

    // VBUS_FAULT_MASK=1, BAT_FAULT_MASK=1, SYS_FAULT_MASK=0 (POR), OTG_FAULT_MASK=1,TSHUT_MASK=0 (POR), RESERVED=00, TS_MASK=1
    reg_bytes[0] = (1U << 7U) | (1U << 6U) | (0U << 5U) | (1U << 4U) | (0U << 3U) | (0U << 2U) | (0U << 1U) | (1U << 0U);

    ret = bq25628_i2c_write(BQ25628_FAULT_MASK, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- ADC Control --- */

    // ADC_EN=0 (disparo manual), ADC_RATE=1 (One-shot), ADC_SAMPLE=00,ADC_AVG=0, ADC_AVG_INIT=0, RESERVED=00
    reg_bytes[0] = (0U << 7U) | (1U << 6U) | (0U << 5U) | (0U << 4U) | (0U << 3U) | (0U << 2U) | (0U << 1U) | (0U << 0U);

    ret = bq25628_i2c_write(BQ25628_ADC_CTRL, reg_bytes, 1);
    if (ret != ESP_OK) return ret;

    /* --- ADC Function --- */

    // Desactivar la lectura de VPMID
    reg_bytes[0] = (0U << 7U) | (0U << 6U) | (0U << 5U) | (0U << 4U) | (0U << 3U) | (0U << 2U) | (0U << 1U) | (1U << 0U);

    ret = bq25628_i2c_write(BQ25628_ADC_FUNCTION, reg_bytes, 1);
    if (ret != ESP_OK) return ret;


    return ESP_OK;
}

esp_err_t bq25628_readADC(bq25628_adc_data_t *adc_data){
	if (adc_data == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(bq25628_adc_sem, 0);	// Limpia cualquier disparo previo pendiente en el semáforo

	uint8_t reg_data;
	reg_data = (1U << 7U) | (1U << 6U) | (0U << 5U) | (0U << 4U) | (0U << 3U) | (0U << 2U) | (0U << 1U) | (0U << 0U);

	esp_err_t ret = bq25628_i2c_write(BQ25628_ADC_CTRL, &reg_data, 1);
    if (ret != ESP_OK) return ret;

    // Bloquea la tarea hasta que la rutina de interrupcion notifique el fin de conversión
    if (xSemaphoreTake(bq25628_adc_sem, pdMS_TO_TICKS(300)) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* --- Input Bus Current (IBUS_ADC) --- */

    uint8_t reg_bytes[2];

    ret = bq25628_i2c_read(BQ25628_ADC_IBUS, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (complemento a 2)
    int16_t ibus_raw = (int16_t)(((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0]);

    // Alineación de bits [15:1] (bit 0 es RESERVED) y conversión a mA (2 mA/LSB)
    int16_t ibus_code = ibus_raw >> 1;
    adc_data->ibus_ma = ibus_code * 2;
    if (adc_data->ibus_ma < 0) adc_data->ibus_ma = 0;

    /* --- Battery Current (IBAT_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_IBAT, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (complemento a 2)
    uint16_t ibat_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Solo guarda si no hubo error por cambio de polaridad durante la conversión (0x8000)
    if (ibat_raw != 0x8000U) {
        // Alineación de bits [15:2] (bits 1:0 son RESERVED) y conversión a mA (4 mA/LSB)
        int16_t ibat_code = (int16_t)ibat_raw >> 2;
        adc_data->ibat_ma = ibat_code * 4;
    }

    /* --- Input Bus Voltage (VBUS_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_VBUS, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (valor sin signo)
    uint16_t vbus_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Alineación de bits [14:2] (bit 15 y bits 1:0 son RESERVED) y conversión a V (3.97 mV/LSB)
    uint16_t vbus_code = (vbus_raw & 0x7FFCU) >> 2U;
    adc_data->vbus_v = (float)vbus_code * 0.00397f;

    /* --- Battery Voltage (VBAT_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_VBAT, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (valor sin signo)
    uint16_t vbat_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Alineación de bits [12:1] (bits 15:13 y bit 0 son RESERVED) y conversión a V (1.99 mV/LSB)
    uint16_t vbat_code = (vbat_raw & 0x1FFEU) >> 1U;
    adc_data->vbat_v = (float)vbat_code * 0.00199f;

    /* --- System Voltage (VSYS_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_VSYS, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (valor sin signo)
    uint16_t vsys_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Alineación de bits [12:1] (bits 15:13 y bit 0 son RESERVED) y conversión a V (1.99 mV/LSB)
    uint16_t vsys_code = (vsys_raw & 0x1FFEU) >> 1U;
    adc_data->vsys_v = (float)vsys_code * 0.00199f;

    /* --- Battery Temperature (TS_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_TS, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian (valor sin signo)
    uint16_t ts_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Alineación de bits [12:1] y conversión a relación V_TS / V_TS_BIAS (0.0961 % / LSB)
    uint16_t ts_code = (ts_raw & 0x1FFEU) >> 1U;
    float ts_ratio = (float)ts_code * 0.000961f;

    // Despeje de R_NTC para red estándar TI: RT1 = 5.23 kOhm, RT2 = 30.1 kOhm
    if (ts_ratio > 0.05f && ts_ratio < 0.845f) {
        float r_bottom = (ts_ratio * 5230.0f) / (1.0f - ts_ratio);
        float r_ntc = (30100.0f * r_bottom) / (30100.0f - r_bottom);

        // Ecuación Beta para NTC 103AT-2 (R0 = 10 kOhm a 25 °C, Beta = 3435 K)
        float inv_t = (1.0f / 298.15f) + (1.0f / 3435.0f) * logf(r_ntc / 10000.0f);
        float temp_c = (1.0f / inv_t) - 273.15f;

        // Conversión a décimas de °C con redondeo al entero más cercano
        float temp_scaled = temp_c * 10.0f;
        adc_data->ts_temp_c = (temp_scaled >= 0.0f) ? (int16_t)(temp_scaled + 0.5f) : (int16_t)(temp_scaled - 0.5f);
    } else adc_data->ts_temp_c = INT16_MIN;    // Falla de sensor: NTC abierto o en cortocircuito

    /* --- IC Die Temperature (TDIE_ADC) --- */

    ret = bq25628_i2c_read(BQ25628_ADC_TDIE, reg_bytes, sizeof(reg_bytes));
    if (ret != ESP_OK) return ret;

    // Desempaquetado de 2 bytes Little-Endian
    uint16_t tdie_raw = ((uint16_t)reg_bytes[1] << 8U) | reg_bytes[0];

    // Extensión de signo de 12 bits [11:0] (bits 15:12 son RESERVED)
    int16_t tdie_code = (int16_t)((tdie_raw & 0x0FFFU) << 4U) >> 4;

    // Conversión a décimas de °C (0.5 °C/LSB = 5 décimas de °C/LSB)
    adc_data->tdie_temp_c = tdie_code * 5;

	return ESP_OK;
}

esp_err_t bq25628_read_faults(uint8_t *faults){
    if (faults == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t fault_status = 0;
    uint8_t fault_flag = 0;
    esp_err_t ret;

    /* --- FAULT Status 0: condición activa en tiempo real --- */
    ret = bq25628_i2c_read(BQ25628_FAULT_STAT, &fault_status, 1);
    if (ret != ESP_OK) return ret;

    /* --- FAULT Flag 0: evento enclavado (Read-to-Clear, libera pin INT) --- */
    ret = bq25628_i2c_read(BQ25628_FAULT_FLAG, &fault_flag, 1);
    if (ret != ESP_OK) return ret;

    uint8_t active_faults = fault_status | fault_flag;
    uint8_t result = BQ25628_FAULT_NONE;

    // Bit 5: SYS_FAULT (sobretensión o cortocircuito en VSYS)
    if (active_faults & (1U << 5U)) {
        result |= BQ25628_FAULT_SYS;
    }

    // Bit 3: TSHUT (apagado térmico del silicio ~150 °C)
    if (active_faults & (1U << 3U)) {
        result |= BQ25628_FAULT_TSHUT;
    }

    *faults = result;
    return ESP_OK;
}

esp_err_t bq25628_read_status(bq25628_status_t *status){
    if (status == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t chg_status_1 = 0;
    uint8_t chg_flag_1 = 0;
    esp_err_t ret;

    /* --- Charger Status 1: estado operativo actual --- */
    ret = bq25628_i2c_read(BQ25628_CHARGER_STAT1, &chg_status_1, 1);
    if (ret != ESP_OK) return ret;

    /* --- Charger Flag 1: lectura Read-to-Clear para liberar INT --- */
    ret = bq25628_i2c_read(BQ25628_CHARGER_FLAG1, &chg_flag_1, 1);
    if (ret != ESP_OK) return ret;

    status->chg_stat = (bq25628_chg_stat_t)((chg_status_1 >> 3U) & 0x03U);

    status->vbus_stat = (bq25628_vbus_stat_t)(chg_status_1 & 0x07U);

    // Presencia de fuente externa válida en VBUS
    status->vbus_present = (status->vbus_stat != BQ25628_VBUS_STAT_NO_INPUT);

    return ESP_OK;
}
