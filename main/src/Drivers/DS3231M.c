/**
 * @file DS3231M.c
 * @brief Implementación del driver I2C para el RTC DS3231M (ESP-IDF i2c_master).
 *
 * Este archivo implementa las primitivas de comunicación I2C protegidas por Mutex
 * de FreeRTOS, conversión BCD <-> Decimal, lectura/escritura atómica de fecha y
 * hora en ráfaga, gestión de alarmas, lectura del sensor de temperatura interno
 * y monitoreo de pérdida de energía mediante la bandera OSF.
 */

#include "DS3231M.h"

/* Tiempo máximo de espera (ms) para completar una transacción física por el bus I2C */
#define DS3231M_I2C_TIMEOUT_MS      100

/* Handle global del dispositivo DS3231M registrado en el bus I2C1 */
i2c_master_dev_handle_t DS3231M_dev = NULL;

/* =========================================================================
 * Funciones auxiliares internas (Mutex, I2C y conversión BCD)
 * ========================================================================= */

/**
 * @brief Toma el semáforo Mutex compartido del bus I2C1 antes de transmitir.
 *
 * Como en I2C1 conviven varios integrados (BQ25628, BQ27441, TCA8418, PCAL6408A,
 * DS3231M y Display) que pueden ser accedidos desde distintas tareas de FreeRTOS,
 * el Mutex evita colisiones en el bus.
 *
 * @return true si obtuvo el Mutex (o si USE_MUTEX == 0), false si expiró el tiempo.
 */
static inline bool ds3231m_lock(void)
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
static inline void ds3231m_unlock(void)
{
#if USE_MUTEX
    if (I2C1_Mutex != NULL) {
        xSemaphoreGive(I2C1_Mutex);
    }
#endif
}

/**
 * @brief Convierte un número decimal (0..99) al formato BCD (Binary-Coded Decimal).
 *
 * El DS3231M almacena los segundos, minutos, horas, día y fecha codificados en BCD
 * (el nibble alto representa las decenas y el nibble bajo las unidades).
 * Ejemplo: 25 decimal -> (2 << 4) | 5 = 0x25.
 *
 * @param[in] val Valor decimal entre 0 y 99.
 * @return Valor codificado en BCD.
 */
static uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

/**
 * @brief Convierte un byte en formato BCD a su valor decimal natural.
 *
 * Ejemplo: 0x59 BCD -> (5 * 10) + 9 = 59 decimal.
 *
 * @param[in] bcd Byte en formato BCD.
 * @return Valor decimal equivalente.
 */
static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

/**
 * @brief Escribe uno o más bytes consecutivos a partir de un registro inicial.
 *
 * Secuencia I2C generada (Figura 9A y 9C del datasheet):
 * [START] -> [Addr+W (0xD0)] -> [start_reg] -> [data[0]] ... [data[len-1]] -> [STOP]
 * El puntero interno de direcciones del DS3231M se autoincrementa tras cada byte.
 *
 * @param[in] start_reg Dirección del primer registro a escribir (0x00 .. 0x10).
 * @param[in] data      Puntero al arreglo de bytes a escribir.
 * @param[in] len       Cantidad de bytes a escribir (máximo 16).
 * @return ESP_OK si la transferencia fue exitosa, o código de error ESP-IDF.
 */
static esp_err_t ds3231m_write_bytes(uint8_t start_reg, const uint8_t *data, size_t len)
{
    /* Verificación defensiva de parámetros y estado de inicialización */
    if (DS3231M_dev == NULL || data == NULL || len == 0 || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Armamos un buffer continuo: byte 0 es la dirección de registro y luego los datos */
    uint8_t tx_buf[17];
    tx_buf[0] = start_reg;
    for (size_t i = 0; i < len; i++) {
        tx_buf[i + 1] = data[i];
    }

    /* Bloqueamos el bus I2C1 para exclusividad de acceso */
    if (!ds3231m_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    /* Transmitimos dirección de registro + payload en una única ráfaga sin interrupciones */
    esp_err_t ret = i2c_master_transmit(DS3231M_dev, tx_buf, len + 1, DS3231M_I2C_TIMEOUT_MS);

    /* Liberamos el bus inmediatamente después de transmitir */
    ds3231m_unlock();
    return ret;
}

/**
 * @brief Lee uno o más bytes consecutivos a partir de un registro inicial.
 *
 * Secuencia I2C generada (Figura 9B y 9D del datasheet):
 * [START] -> [Addr+W (0xD0)] -> [start_reg] -> [REPEATED START] ->
 * [Addr+R (0xD1)] -> [data[0] + ACK] ... [data[len-1] + NACK] -> [STOP]
 *
 * @param[in]  start_reg Dirección del primer registro a leer.
 * @param[out] data      Buffer donde se guardarán los bytes leídos.
 * @param[in]  len       Cantidad de bytes a leer.
 * @return ESP_OK si la lectura fue exitosa, o código de error ESP-IDF.
 */
static esp_err_t ds3231m_read_bytes(uint8_t start_reg, uint8_t *data, size_t len)
{
    if (DS3231M_dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ds3231m_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    /* i2c_master_transmit_receive ejecuta escritura de puntero + Repeated START + lectura */
    esp_err_t ret = i2c_master_transmit_receive(
        DS3231M_dev,
        &start_reg,
        1,
        data,
        len,
        DS3231M_I2C_TIMEOUT_MS
    );

    ds3231m_unlock();
    return ret;
}

/* =========================================================================
 * Funciones públicas del driver
 * ========================================================================= */

esp_err_t ds3231m_init(i2c_master_bus_handle_t bus_handle)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configuración del esclavo en el bus maestro de ESP-IDF (7 bits, 400 kHz) */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231M_I2C_ADDR,
        .scl_speed_hz    = DS3231M_I2C_FREQ_HZ,
    };

    /* Registra el dispositivo y guarda su handle en DS3231M_dev */
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &DS3231M_dev);
}

esp_err_t ds3231m_config(const ds3231m_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Construir el byte del Registro de Control (0x0E) según las opciones elegidas */
    uint8_t ctrl_reg = 0x00;

    /* Nota: EOSC tiene lógica invertida (0 = oscilador encendido en VBAT, 1 = apagado) */
    if (!cfg->enable_osc_on_vbat) {
        ctrl_reg |= DS3231M_CTRL_EOSC; // | esta operacion es equivalente a ctrl_reg = ctrl_reg (OR) DS3231M_CTRL_EOSC.
    }
    if (cfg->enable_sqw_on_vbat) {
        ctrl_reg |= DS3231M_CTRL_BBSQW;
    }
    if (cfg->int_pin_alarm_mode) {
        ctrl_reg |= DS3231M_CTRL_INTCN;
    }
    if (cfg->enable_alarm2_int) {
        ctrl_reg |= DS3231M_CTRL_A2IE;
    }
    if (cfg->enable_alarm1_int) {
        ctrl_reg |= DS3231M_CTRL_A1IE;
    }

    esp_err_t ret = ds3231m_write_bytes(DS3231M_REG_CONTROL, &ctrl_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. Actualizar EN32KHZ en el Registro de Estado (0x0F) mediante lectura-modificación-escritura
     *    para no pisar accidentalmente las banderas OSF, A1F o A2F */
    uint8_t status_reg = 0;
    ret = ds3231m_read_bytes(DS3231M_REG_STATUS, &status_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    // Habilitamos o deshabilitamos la salida de 32.768 kHz según la configuración
    if (cfg->enable_32khz_out) {
        status_reg |= DS3231M_STAT_EN32KHZ;
    } else {
        status_reg &= (uint8_t)(~DS3231M_STAT_EN32KHZ); //~ eso es un operador de negacion, invierte los bits de la mascara, para que al hacer el AND con status_reg se borre el bit correspondiente a EN32KHZ.
    }

    return ds3231m_write_bytes(DS3231M_REG_STATUS, &status_reg, 1);
}

esp_err_t ds3231m_set_time(const ds3231m_time_t *time)
{
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validación estricta de rangos: el datasheet advierte funcionamiento indefinido
     * si se cargan valores ilógicos (ej. mes 13 o día 32) */
    if (time->seconds > 59 ||
        time->minutes > 59 ||
        time->hours > 23 ||
        time->day_of_week < 1 || time->day_of_week > 7 ||
        time->date < 1 || time->date > 31 ||
        time->month < 1 || time->month > 12 ||
        time->year < 2000 || time->year > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Preparamos los 7 registros consecutivos (0x00 a 0x06) en formato BCD */
    uint8_t buf[7];
    buf[0] = dec_to_bcd(time->seconds);
    buf[1] = dec_to_bcd(time->minutes);
    buf[2] = dec_to_bcd(time->hours) & 0x3FU;               /* Bit 6 = 0 fuerza formato 24 horas */
    buf[3] = dec_to_bcd(time->day_of_week) & 0x07U;         /* Bits 2:0 (1..7) */
    buf[4] = dec_to_bcd(time->date) & 0x3FU;                /* Bits 5:0 (01..31) */
    buf[5] = dec_to_bcd(time->month) & 0x1FU;               /* Bit 7 (Century) = 0 para años 2000..2099 */
    buf[6] = dec_to_bcd((uint8_t)(time->year - 2000U));     /* Últimos dos dígitos del año (00..99) */

    /* Escribimos los 7 registros en una sola ráfaga I2C.
     * Al escribir 0x00 (segundos) se reinicia la cadena interna de divisores y
     * el datasheet exige escribir el resto dentro de 1 segundo (aquí tarda < 0.3 ms) */
    esp_err_t ret = ds3231m_write_bytes(DS3231M_REG_SECONDS, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    /* Una vez cargada una hora válida, limpiamos la bandera de oscilador detenido (OSF) */
    return ds3231m_clear_osf();
}

esp_err_t ds3231m_get_time(ds3231m_time_t *time)
{
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Leemos los 7 registros (0x00..0x06) en una sola transacción.
     * En el instante del START I2C, el DS3231M congela la hora en un buffer secundario
     * interno para evitar errores si cambia de segundo mientras leemos byte a byte */
    uint8_t buf[7];
    esp_err_t ret = ds3231m_read_bytes(DS3231M_REG_SECONDS, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    /* Enmascaramos los bits reservados/de control de cada byte antes de decodificar BCD */
    time->seconds = bcd_to_dec(buf[0] & 0x7FU);
    time->minutes = bcd_to_dec(buf[1] & 0x7FU);

    /* Decodificación de horas: contemplamos ambos modos (12h y 24h) por seguridad */
    if (buf[2] & (1U << 6)) {
        /* Bit 6 = 1 -> Modo 12 horas: bit 5 indica PM (1) o AM (0), bits 4:0 hora 1..12 */
        uint8_t hr12 = bcd_to_dec(buf[2] & 0x1FU);
        bool is_pm   = ((buf[2] & (1U << 5)) != 0);
        if (hr12 == 12) {
            time->hours = is_pm ? 12 : 0;
        } else {
            time->hours = is_pm ? (hr12 + 12) : hr12;
        }
    } else {
        /* Bit 6 = 0 -> Modo 24 horas: bits 5:0 contienen la hora 00..23 */
        time->hours = bcd_to_dec(buf[2] & 0x3FU);
    }

    time->day_of_week = bcd_to_dec(buf[3] & 0x07U);
    time->date        = bcd_to_dec(buf[4] & 0x3FU);
    time->month       = bcd_to_dec(buf[5] & 0x1FU);
    time->year        = (uint16_t)(2000U + bcd_to_dec(buf[6]));

    return ESP_OK;
}

esp_err_t ds3231m_is_oscillator_stopped(bool *is_stopped)
{
    if (is_stopped == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status_reg = 0;
    esp_err_t ret = ds3231m_read_bytes(DS3231M_REG_STATUS, &status_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Si el bit 7 (OSF) está en 1, el RTC perdió alimentación o se detuvo en algún momento */
    *is_stopped = ((status_reg & DS3231M_STAT_OSF) != 0);
    return ESP_OK;
}

esp_err_t ds3231m_clear_osf(void)
{
    uint8_t status_reg = 0;
    esp_err_t ret = ds3231m_read_bytes(DS3231M_REG_STATUS, &status_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Ponemos OSF en 0 para limpiar el aviso de corte de energía.
     * Importante: según el datasheet, escribir '1' en A1F y A2F NO los altera (solo se
     * borran escribiendo '0'), así que los forzamos en 1 para no perder alarmas pendientes */
    status_reg &= (uint8_t)(~DS3231M_STAT_OSF);
    status_reg |= (DS3231M_STAT_A1F | DS3231M_STAT_A2F);

    return ds3231m_write_bytes(DS3231M_REG_STATUS, &status_reg, 1);
}

esp_err_t ds3231m_get_temperature(float *temp_c)
{
    if (temp_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Leemos los registros 0x11 (MSB: parte entera con signo) y 0x12 (LSB: bits 7:6 fracción) */
    uint8_t buf[2];
    esp_err_t ret = ds3231m_read_bytes(DS3231M_REG_TEMP_MSB, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    /* Reconstruimos el entero con signo de 10 bits en complemento a dos:
     * Casteamos buf[0] a int8_t para propagar el bit de signo al desplazar 2 lugares a la izquierda,
     * y sumamos los 2 bits más significativos de buf[1]. Cada LSB equivale a 0.25 °C */
    int16_t raw = (int16_t)(((int16_t)((int8_t)buf[0]) << 2) | (buf[1] >> 6));
    *temp_c = (float)raw * 0.25f;

    return ESP_OK;
}

esp_err_t ds3231m_set_alarm1(const ds3231m_alarm1_t *alarm)
{
    if (alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (alarm->seconds > 59 || alarm->minutes > 59 || alarm->hours > 23) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validamos day_or_date solo cuando la tasa seleccionada lo utiliza */
    bool use_day_of_week = (alarm->rate == DS3231M_A1_MATCH_DAY_HOUR_MIN_SEC);
    if (use_day_of_week) {
        if (alarm->day_or_date < 1 || alarm->day_or_date > 7) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (alarm->rate == DS3231M_A1_MATCH_DATE_HOUR_MIN_SEC) {
        if (alarm->day_or_date < 1 || alarm->day_or_date > 31) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Configuramos los bits de máscara A1M1..A1M4 (bit 7 de los registros 0x07..0x0A)
     * siguiendo exactamente la Tabla 3 del datasheet */
    uint8_t a1m1 = 0, a1m2 = 0, a1m3 = 0, a1m4 = 0;
    switch (alarm->rate) {
        case DS3231M_A1_EVERY_SECOND:
            a1m1 = 1; a1m2 = 1; a1m3 = 1; a1m4 = 1;
            break;
        case DS3231M_A1_MATCH_SECONDS:
            a1m2 = 1; a1m3 = 1; a1m4 = 1;
            break;
        case DS3231M_A1_MATCH_MIN_SEC:
            a1m3 = 1; a1m4 = 1;
            break;
        case DS3231M_A1_MATCH_HOUR_MIN_SEC:
            a1m4 = 1;
            break;
        case DS3231M_A1_MATCH_DATE_HOUR_MIN_SEC:
        case DS3231M_A1_MATCH_DAY_HOUR_MIN_SEC:
            /* Todos los bits de máscara en 0: exige coincidencia completa */
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    /* Armamos los 4 bytes de Alarma 1 (0x07 .. 0x0A) */
    uint8_t buf[4];
    buf[0] = (dec_to_bcd(alarm->seconds) & 0x7FU) | (a1m1 << 7);
    buf[1] = (dec_to_bcd(alarm->minutes) & 0x7FU) | (a1m2 << 7);
    buf[2] = (dec_to_bcd(alarm->hours)   & 0x3FU) | (a1m3 << 7); /* Bit 6 = 0 -> Modo 24h */
    buf[3] = (dec_to_bcd(alarm->day_or_date) & 0x3FU) |
             ((use_day_of_week ? 1U : 0U) << 6) |                 /* Bit 6 (DY/DT): 1=Día semana, 0=Fecha mes */
             (a1m4 << 7);

    return ds3231m_write_bytes(DS3231M_REG_ALARM1_SEC, buf, sizeof(buf));
}

esp_err_t ds3231m_set_alarm2(const ds3231m_alarm2_t *alarm)
{
    if (alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (alarm->minutes > 59 || alarm->hours > 23) {
        return ESP_ERR_INVALID_ARG;
    }

    bool use_day_of_week = (alarm->rate == DS3231M_A2_MATCH_DAY_HOUR_MIN);
    if (use_day_of_week) {
        if (alarm->day_or_date < 1 || alarm->day_or_date > 7) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (alarm->rate == DS3231M_A2_MATCH_DATE_HOUR_MIN) {
        if (alarm->day_or_date < 1 || alarm->day_or_date > 31) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Configuramos los bits de máscara A2M2..A2M4 (Alarma 2 no tiene registro de segundos;
     * siempre dispara en el segundo 00) según la Tabla 3 del datasheet */
    uint8_t a2m2 = 0, a2m3 = 0, a2m4 = 0;
    switch (alarm->rate) {
        case DS3231M_A2_EVERY_MINUTE:
            a2m2 = 1; a2m3 = 1; a2m4 = 1;
            break;
        case DS3231M_A2_MATCH_MINUTES:
            a2m3 = 1; a2m4 = 1;
            break;
        case DS3231M_A2_MATCH_HOUR_MIN:
            a2m4 = 1;
            break;
        case DS3231M_A2_MATCH_DATE_HOUR_MIN:
        case DS3231M_A2_MATCH_DAY_HOUR_MIN:
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    /* Armamos los 3 bytes de Alarma 2 (0x0B .. 0x0D) */
    uint8_t buf[3];
    buf[0] = (dec_to_bcd(alarm->minutes) & 0x7FU) | (a2m2 << 7);
    buf[1] = (dec_to_bcd(alarm->hours)   & 0x3FU) | (a2m3 << 7); /* Bit 6 = 0 -> Modo 24h */
    buf[2] = (dec_to_bcd(alarm->day_or_date) & 0x3FU) |
             ((use_day_of_week ? 1U : 0U) << 6) |                 /* Bit 6 (DY/DT) */
             (a2m4 << 7);

    return ds3231m_write_bytes(DS3231M_REG_ALARM2_MIN, buf, sizeof(buf));
}

esp_err_t ds3231m_check_and_clear_alarms(bool *alarm1_triggered, bool *alarm2_triggered, bool clear_flags)
{
    uint8_t status_reg = 0;
    esp_err_t ret = ds3231m_read_bytes(DS3231M_REG_STATUS, &status_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Extraemos el estado actual de las banderas A1F (bit 0) y A2F (bit 1) */
    bool a1 = ((status_reg & DS3231M_STAT_A1F) != 0);
    bool a2 = ((status_reg & DS3231M_STAT_A2F) != 0);

    if (alarm1_triggered != NULL) {
        *alarm1_triggered = a1;
    }
    if (alarm2_triggered != NULL) {
        *alarm2_triggered = a2;
    }

    /* Si se pidió limpiar y al menos una bandera estaba activa, escribimos 0 en A1F y A2F.
     * Esto además libera el pin físico INT/SQW (deja de tirar a masa) para futuros eventos */
    if (clear_flags && (a1 || a2)) {
        status_reg &= (uint8_t)(~(DS3231M_STAT_A1F | DS3231M_STAT_A2F));
        status_reg |= DS3231M_STAT_OSF; /* Preservamos OSF intacto */
        ret = ds3231m_write_bytes(DS3231M_REG_STATUS, &status_reg, 1);
    }

    return ret;
}