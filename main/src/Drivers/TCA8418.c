/**
 * @file TCA8418.c
 * @brief Implementación del driver I2C para el controlador de teclado TCA8418.
 *
 * Implementa la comunicación I2C protegida mediante Mutex de FreeRTOS,
 * configuración de matriz filas/columnas, lectura y decodificación de eventos
 * desde la FIFO de 10 posiciones, y limpieza controlada de interrupciones.
 */

#include "TCA8418.h"

/* Tiempo máximo de espera (ms) para completar una transferencia física I2C */
#define TCA8418_I2C_TIMEOUT_MS      100

/* Nota: La variable global `i2c_master_dev_handle_t TCA8418_dev = NULL;` se define
 * en app_main.c siguiendo la convención de dispositivos del bus I2C1 del proyecto. */

/* =========================================================================
 * Funciones auxiliares internas (Sincronización Mutex y primitivas I2C)
 * ========================================================================= */

/**
 * @brief Adquiere el semáforo Mutex compartido del bus I2C1 antes de transmitir.
 *
 * Evita colisiones en el bus I2C1 cuando distintas tareas acceden simultáneamente
 * al cargador BQ25628, fuel gauge BQ27441, RTC DS3231M, expansor GPIO o teclado.
 *
 * @return true si obtuvo el Mutex (o si USE_MUTEX == 0), false si expiró el timeout.
 */
static inline bool tca8418_lock(void)
{
#if USE_MUTEX
    if (I2C1_Mutex != NULL) {
        return (xSemaphoreTake(I2C1_Mutex, pdMS_TO_TICKS(I2C_MUTEX_TIMEOUT_MS)) == pdTRUE);
    }
#endif
    return true;
}

/**
 * @brief Libera el semáforo Mutex del bus I2C1 al finalizar la transacción.
 */
static inline void tca8418_unlock(void)
{
#if USE_MUTEX
    if (I2C1_Mutex != NULL) {
        xSemaphoreGive(I2C1_Mutex);
    }
#endif
}

/**
 * @brief Escribe un único byte en un registro interno del TCA8418.
 *
 * Secuencia I2C generada (Figura 23 del datasheet):
 * [START] -> [Addr+W (0x68)] -> [reg] -> [val] -> [STOP]
 *
 * @param[in] reg Dirección del registro destino (0x01 .. 0x2E).
 * @param[in] val Valor de 8 bits a escribir.
 * @return ESP_OK si la transferencia fue exitosa, o código de error ESP-IDF.
 */
static esp_err_t tca8418_write_reg(uint8_t reg, uint8_t val)
{
    /* Los registros 0x00 y 0x2F son reservados y no generan ACK según el datasheet */
    if (TCA8418_dev == NULL || reg == 0x00 || reg >= 0x2F) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Armamos la trama de 2 bytes: dirección de registro + dato */
    uint8_t tx_buf[2] = { reg, val };

    if (!tca8418_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2c_master_transmit(TCA8418_dev, tx_buf, sizeof(tx_buf), TCA8418_I2C_TIMEOUT_MS);

    tca8418_unlock();
    return ret;
}

/**
 * @brief Lee un único byte desde un registro interno del TCA8418.
 *
 * Secuencia I2C generada (Figura 25 del datasheet):
 * [START] -> [Addr+W (0x68)] -> [reg] -> [REPEATED START] -> [Addr+R (0x69)] -> [data + NACK] -> [STOP]
 *
 * @param[in]  reg Dirección del registro a leer (0x01 .. 0x2E).
 * @param[out] val Puntero donde se almacenará el byte leído.
 * @return ESP_OK si la lectura fue exitosa, o código de error ESP-IDF.
 */
static esp_err_t tca8418_read_reg(uint8_t reg, uint8_t *val)
{
    if (TCA8418_dev == NULL || val == NULL || reg == 0x00 || reg >= 0x2F) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!tca8418_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2c_master_transmit_receive(
        TCA8418_dev,
        &reg,
        1,
        val,
        1,
        TCA8418_I2C_TIMEOUT_MS
    );

    tca8418_unlock();
    return ret;
}

/* =========================================================================
 * Funciones públicas del driver
 * ========================================================================= */

esp_err_t tca8418_init(i2c_master_bus_handle_t bus_handle)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configuración del dispositivo esclavo en el controlador maestro ESP-IDF */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA8418_I2C_ADDR,
        .scl_speed_hz    = TCA8418_I2C_FREQ_HZ,
    };

    /* Registra el TCA8418 en I2C1 y guarda su handle global en TCA8418_dev */
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &TCA8418_dev);
}

esp_err_t tca8418_config(const tca8418_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validamos que cols_mask no utilice bits más allá de COL9 (10 columnas: bits 0..9) */
    if ((cfg->cols_mask & ~0x03FFU) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    /* 1. Asignar qué pines pertenecen a la matriz de teclado (1 = Matriz, 0 = GPIO)
     *    KP_GPIO1 (0x1D): ROW0..ROW7
     *    KP_GPIO2 (0x1E): COL0..COL7 (byte bajo de cols_mask)
     *    KP_GPIO3 (0x1F): COL8..COL9 (bits 9:8 de cols_mask desplazados a bits 1:0) */
    uint8_t kp_gpio1 = cfg->rows_mask;
    uint8_t kp_gpio2 = (uint8_t)(cfg->cols_mask & 0xFFU);
    uint8_t kp_gpio3 = (uint8_t)((cfg->cols_mask >> 8) & 0x03U);

    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO1, kp_gpio1);
    if (ret != ESP_OK) return ret;

    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO2, kp_gpio2);
    if (ret != ESP_OK) return ret;

    ret = tca8418_write_reg(TCA8418_REG_KP_GPIO3, kp_gpio3);
    if (ret != ESP_OK) return ret;

    /* 2. Configurar dirección de pines que quedaron en modo GPIO como entradas seguras (0 = Input)
     *    y deshabilitar interrupciones GPI individuales (se usan solo eventos de matriz) */
    ret = tca8418_write_reg(TCA8418_REG_GPIO_DIR1, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_DIR2, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_DIR3, 0x00);
    if (ret != ESP_OK) return ret;

    ret = tca8418_write_reg(TCA8418_REG_GPIO_INT_EN1, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_INT_EN2, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_INT_EN3, 0x00);
    if (ret != ESP_OK) return ret;

    /* 3. Configurar filtro antirrebote por hardware (DEBOUNCE_DIS1..3):
     *    Nota: Lógica invertida en el chip -> 0 = Debounce HABILITADO, 1 = DESHABILITADO */
    uint8_t deb_val = cfg->enable_debounce ? 0x00U : 0xFFU;
    ret = tca8418_write_reg(TCA8418_REG_DEBOUNCE_DIS1, deb_val);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_DEBOUNCE_DIS2, deb_val);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_DEBOUNCE_DIS3, deb_val & 0x03U);
    if (ret != ESP_OK) return ret;

    /* 4. Configurar resistencias pull-up internas (~100 kOhm) en GPIO_PULL1..3:
     *    Nota: Lógica invertida en el chip -> 0 = Pull-up HABILITADO, 1 = DESHABILITADO */
    uint8_t pull_val = cfg->enable_pullups ? 0x00U : 0xFFU;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_PULL1, pull_val);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_PULL2, pull_val);
    if (ret != ESP_OK) return ret;
    ret = tca8418_write_reg(TCA8418_REG_GPIO_PULL3, pull_val & 0x03U);
    if (ret != ESP_OK) return ret;

    /* 5. Construir el Registro de Configuración CFG (0x01) */
    uint8_t cfg_reg = 0x00U;

    if (cfg->overflow_wrap) {
        cfg_reg |= TCA8418_CFG_OVR_FLOW_M;
    }
    if (cfg->int_deassert_50us) {
        /* Genera un pulso de 50 us en alto al limpiar INT si aún quedan teclas en la FIFO,
         * garantizando un nuevo flanco descendente para la interrupción del ESP32 */
        cfg_reg |= TCA8418_CFG_INT_CFG;
    }
    if (cfg->enable_overflow_int) {
        /* Según la sección 8.6.4 (Overflow Errata) del datasheet, OVR_FLOW_IEN requiere
         * que OVR_FLOW_M también esté en 1 para disparar la interrupción */
        cfg_reg |= (TCA8418_CFG_OVR_FLOW_IEN | TCA8418_CFG_OVR_FLOW_M);
    }
    if (cfg->enable_gpi_int) {
        cfg_reg |= TCA8418_CFG_GPI_IEN;
    }
    if (cfg->enable_key_int) {
        cfg_reg |= TCA8418_CFG_KE_IEN;
    }

    ret = tca8418_write_reg(TCA8418_REG_CFG, cfg_reg);
    if (ret != ESP_OK) return ret;

    /* 6. Limpiar eventos residuales en FIFO y banderas previas en INT_STAT */
    return tca8418_flush_fifo(NULL);
}

esp_err_t tca8418_get_event_count(uint8_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_val = 0;
    esp_err_t ret = tca8418_read_reg(TCA8418_REG_KEY_LCK_EC, &reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Los 4 bits menos significativos KEC[3:0] indican cuántos eventos hay en la FIFO (0..10) */
    *count = (reg_val & TCA8418_FIFO_COUNT_MASK);
    return ESP_OK;
}

esp_err_t tca8418_read_event(tca8418_key_event_t *event, bool *has_event)
{
    if (event == NULL || has_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Inicializamos la estructura de salida en cero por seguridad */
    event->raw_byte     = 0;
    event->key_code     = 0;
    event->is_pressed   = false;
    event->is_gpi_event = false;
    event->row          = 0;
    event->col          = 0;
    *has_event          = false;

    /* Leemos la cabeza de la FIFO en KEY_EVENT_A (0x04).
     * Cada lectura desplaza automáticamente los eventos siguientes hacia 0x04.
     * Si devuelve 0x00, la FIFO está vacía. */
    uint8_t raw = 0;
    esp_err_t ret = tca8418_read_reg(TCA8418_REG_KEY_EVENT_A, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    if (raw == 0x00U) {
        return ESP_OK;
    }

    *has_event        = true;
    event->raw_byte   = raw;
    event->is_pressed = ((raw & TCA8418_EVENT_PRESS_MASK) != 0); /* Bit 7: 1 = Press, 0 = Release */
    event->key_code   = (raw & TCA8418_EVENT_KEY_MASK);          /* Bits 6:0: Código de tecla */

    /* Decodificamos fila y columna según la Tabla 1 del datasheet:
     * En matriz, código = (fila * 10) + columna + 1 (rango 1..80).
     * En GPI, código = 97..114. */
    if (event->key_code >= 1U && event->key_code <= 80U) {
        event->is_gpi_event = false;
        event->row = (uint8_t)((event->key_code - 1U) / 10U);
        event->col = (uint8_t)((event->key_code - 1U) % 10U);
    } else {
        event->is_gpi_event = true;
    }

    return ESP_OK;
}

esp_err_t tca8418_flush_fifo(uint8_t *flushed_count)
{
    uint8_t discarded = 0;
    tca8418_key_event_t dummy_evt;
    bool has_evt = false;
    esp_err_t ret;

    /* Leemos KEY_EVENT_A hasta vaciar la FIFO (máximo 10 iteraciones + 1 de margen) */
    for (uint8_t i = 0; i < (TCA8418_FIFO_MAX_SIZE + 2U); i++) {
        ret = tca8418_read_event(&dummy_evt, &has_evt);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!has_evt) {
            break;
        }
        discarded++;
    }

    if (flushed_count != NULL) {
        *flushed_count = discarded;
    }

    /* Limpiamos todas las banderas de interrupción pendientes en INT_STAT escribiendo '1' */
    return tca8418_write_reg(TCA8418_REG_INT_STAT, 0x1FU);
}

esp_err_t tca8418_get_and_clear_int_status(tca8418_int_status_t *status, bool clear_flags)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t stat_reg = 0;
    esp_err_t ret = tca8418_read_reg(TCA8418_REG_INT_STAT, &stat_reg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Decodificamos cada bandera activa del registro INT_STAT (0x02) */
    status->key_event    = ((stat_reg & TCA8418_INT_KEY) != 0);
    status->gpi_event    = ((stat_reg & TCA8418_INT_GPI) != 0);
    status->keypad_lock  = ((stat_reg & TCA8418_INT_K_LCK) != 0);
    status->overflow     = ((stat_reg & TCA8418_INT_OVR_FLOW) != 0);
    status->cad_sequence = ((stat_reg & TCA8418_INT_CAD) != 0);

    /* Si hay interrupción GPI activa, leer GPIO_INT_STAT1..3 para liberar el flag interno */
    if (status->gpi_event) {
        uint8_t dummy_gpi = 0;
        (void)tca8418_read_reg(TCA8418_REG_GPIO_INT_STAT1, &dummy_gpi);
        (void)tca8418_read_reg(TCA8418_REG_GPIO_INT_STAT2, &dummy_gpi);
        (void)tca8418_read_reg(TCA8418_REG_GPIO_INT_STAT3, &dummy_gpi);
    }

    /* En INT_STAT (0x02), las banderas son W1C (Write-1-to-Clear): se borran escribiendo '1'
     * en los bits que estaban activos (nota: K_INT solo se borra si la FIFO ya está vacía) */
    if (clear_flags && (stat_reg & 0x1FU) != 0) {
        ret = tca8418_write_reg(TCA8418_REG_INT_STAT, (stat_reg & 0x1FU));
    }

    return ret;
}

esp_err_t tca8418_set_keypad_lock(bool lock)
{
    uint8_t reg_val = 0;
    esp_err_t ret = tca8418_read_reg(TCA8418_REG_KEY_LCK_EC, &reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Modificamos únicamente el bit 6 (K_LCK_EN) preservando el resto */
    if (lock) {
        reg_val |= TCA8418_LCK_EN;
    } else {
        reg_val &= (uint8_t)(~TCA8418_LCK_EN);
    }

    return tca8418_write_reg(TCA8418_REG_KEY_LCK_EC, reg_val);
}

esp_err_t tca8418_configure_unlock_sequence(uint8_t key1_code, uint8_t key2_code,
                                            uint8_t unlock_timeout_s, uint8_t mask_timeout_s)
{
    /* Validación según rangos de las secciones 8.6.2.5 y 8.6.2.6 del datasheet:
     * - Teclas válidas: 1..114
     * - Timer entre tecla 1 y tecla 2 (KL[2:0]): 1..7 segundos (0 deshabilita bloqueo)
     * - Timer de máscara de interrupciones (KL[7:3]): 0..31 segundos */
    if (key1_code == 0 || key1_code > 114 ||
        key2_code == 0 || key2_code > 114 ||
        unlock_timeout_s == 0 || unlock_timeout_s > 7 ||
        mask_timeout_s > 31) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = tca8418_write_reg(TCA8418_REG_UNLOCK1, key1_code & 0x7FU);
    if (ret != ESP_OK) return ret;

    ret = tca8418_write_reg(TCA8418_REG_UNLOCK2, key2_code & 0x7FU);
    if (ret != ESP_OK) return ret;

    /* Empaquetamos ambos temporizadores en el registro KP_LCK_TIMER (0x0E):
     * Bits [7:3] = mask_timeout_s, Bits [2:0] = unlock_timeout_s */
    uint8_t timer_reg = (uint8_t)(((mask_timeout_s & 0x1FU) << 3) | (unlock_timeout_s & 0x07U));
    return tca8418_write_reg(TCA8418_REG_KP_LCK_TIMER, timer_reg);
}