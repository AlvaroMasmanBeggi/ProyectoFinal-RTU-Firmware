/**
 * @file DS3231M.h
 * @brief Driver I2C para el RTC de alta precisión DS3231M (ESP-IDF i2c_master).
 */

#ifndef DS3231M_H
#define DS3231M_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Configuración del bus y sincronización (misma convención que BQ25628)
 * ========================================================================= */
#define DS3231M_I2C_ADDR            0x68    /**< Dirección I2C de 7 bits (0xD0 >> 1) */
#define DS3231M_I2C_FREQ_HZ         400000  /**< Fast-mode I2C hasta 400 kHz */

#ifndef USE_MUTEX
#define USE_MUTEX                   1
#endif

#ifndef I2C_MUTEX_TIMEOUT_MS
#define I2C_MUTEX_TIMEOUT_MS        100
#endif


extern SemaphoreHandle_t I2C1_Mutex;
extern i2c_master_dev_handle_t DS3231M_dev;


/* =========================================================================
 * Mapa de registros del DS3231M (0x00 - 0x12)
 * ========================================================================= */
#define DS3231M_REG_SECONDS         0x00
#define DS3231M_REG_MINUTES         0x01
#define DS3231M_REG_HOURS           0x02
#define DS3231M_REG_DAY             0x03
#define DS3231M_REG_DATE            0x04
#define DS3231M_REG_MONTH           0x05
#define DS3231M_REG_YEAR            0x06

#define DS3231M_REG_ALARM1_SEC      0x07
#define DS3231M_REG_ALARM1_MIN      0x08
#define DS3231M_REG_ALARM1_HOUR     0x09
#define DS3231M_REG_ALARM1_DAY_DATE 0x0A

#define DS3231M_REG_ALARM2_MIN      0x0B
#define DS3231M_REG_ALARM2_HOUR     0x0C
#define DS3231M_REG_ALARM2_DAY_DATE 0x0D

#define DS3231M_REG_CONTROL         0x0E
#define DS3231M_REG_STATUS          0x0F
#define DS3231M_REG_AGING_OFFSET    0x10
#define DS3231M_REG_TEMP_MSB        0x11
#define DS3231M_REG_TEMP_LSB        0x12

/* =========================================================================
 * Máscaras de bits - Registro de Control (0x0E) y Estado (0x0F)
 * ========================================================================= */
#define DS3231M_CTRL_EOSC           (1U << 7) /**< 0: Oscilador activo en VBAT, 1: Detenido en VBAT */
#define DS3231M_CTRL_BBSQW          (1U << 6) /**< 1: Habilita onda cuadrada en INT/SQW bajo VBAT */
#define DS3231M_CTRL_CONV           (1U << 5) /**< 1: Fuerza conversión de temperatura */
#define DS3231M_CTRL_INTCN          (1U << 2) /**< 1: Pin INT/SQW actúa como interrupción de alarma */
#define DS3231M_CTRL_A2IE           (1U << 1) /**< 1: Habilita interrupción de Alarma 2 */
#define DS3231M_CTRL_A1IE           (1U << 0) /**< 1: Habilita interrupción de Alarma 1 */

#define DS3231M_STAT_OSF            (1U << 7) /**< 1: El oscilador se detuvo (hora no válida) */
#define DS3231M_STAT_EN32KHZ        (1U << 3) /**< 1: Habilita salida 32.768 kHz */
#define DS3231M_STAT_BSY            (1U << 2) /**< 1: Conversión de temperatura en curso */
#define DS3231M_STAT_A2F            (1U << 1) /**< 1: Bandera de disparo Alarma 2 */
#define DS3231M_STAT_A1F            (1U << 0) /**< 1: Bandera de disparo Alarma 1 */

/* =========================================================================
 * Tipos de datos y estructuras
 * ========================================================================= */

/**
 * @brief Estructura de fecha y hora en formato 24 horas.
 */
typedef struct {
    uint8_t  seconds;       /**< Segundos: 0 - 59 */
    uint8_t  minutes;       /**< Minutos: 0 - 59 */
    uint8_t  hours;         /**< Horas (formato 24h): 0 - 23 */
    uint8_t  day_of_week;   /**< Día de la semana: 1 - 7 (ej. 1 = Domingo .. 7 = Sábado) */
    uint8_t  date;          /**< Día del mes: 1 - 31 */
    uint8_t  month;         /**< Mes: 1 - 12 */
    uint16_t year;          /**< Año completo: 2000 - 2099 */
} ds3231m_time_t;

/**
 * @brief Modos de disparo para la Alarma 1 (Tabla 3 del datasheet).
 */
typedef enum {
    DS3231M_A1_EVERY_SECOND = 0,        /**< Dispara cada segundo */
    DS3231M_A1_MATCH_SECONDS,           /**< Coincide segundos */
    DS3231M_A1_MATCH_MIN_SEC,           /**< Coincide minutos y segundos */
    DS3231M_A1_MATCH_HOUR_MIN_SEC,      /**< Coincide horas, minutos y segundos */
    DS3231M_A1_MATCH_DATE_HOUR_MIN_SEC, /**< Coincide fecha del mes, horas, minutos y segundos */
    DS3231M_A1_MATCH_DAY_HOUR_MIN_SEC   /**< Coincide día de semana, horas, minutos y segundos */
} ds3231m_alarm1_rate_t;

/**
 * @brief Modos de disparo para la Alarma 2 (Tabla 3 del datasheet).
 */
typedef enum {
    DS3231M_A2_EVERY_MINUTE = 0,        /**< Dispara cada minuto (en el segundo 00) */
    DS3231M_A2_MATCH_MINUTES,           /**< Coincide minutos */
    DS3231M_A2_MATCH_HOUR_MIN,          /**< Coincide horas y minutos */
    DS3231M_A2_MATCH_DATE_HOUR_MIN,     /**< Coincide fecha del mes, horas y minutos */
    DS3231M_A2_MATCH_DAY_HOUR_MIN       /**< Coincide día de semana, horas y minutos */
} ds3231m_alarm2_rate_t;

/**
 * @brief Configuración de Alarma 1.
 */
typedef struct {
    uint8_t               seconds;      /**< 0 - 59 */
    uint8_t               minutes;      /**< 0 - 59 */
    uint8_t               hours;        /**< 0 - 23 */
    uint8_t               day_or_date;  /**< Día (1 - 7) o Fecha (1 - 31) según rate */
    ds3231m_alarm1_rate_t rate;         /**< Frecuencia/condición de disparo */
} ds3231m_alarm1_t;

/**
 * @brief Configuración de Alarma 2.
 */
typedef struct {
    uint8_t               minutes;      /**< 0 - 59 */
    uint8_t               hours;        /**< 0 - 23 */
    uint8_t               day_or_date;  /**< Día (1 - 7) o Fecha (1 - 31) según rate */
    ds3231m_alarm2_rate_t rate;         /**< Frecuencia/condición de disparo */
} ds3231m_alarm2_t;

/**
 * @brief Configuración operativa inicial del DS3231M.
 */
typedef struct {
    bool enable_osc_on_vbat;    /**< true: mantiene oscilador corriendo con batería (EOSC = 0) */
    bool enable_32khz_out;      /**< true: habilita salida 32.768 kHz (false ahorra batería) */
    bool int_pin_alarm_mode;    /**< true: INT/SQW como interrupción de alarma; false: onda 1 Hz */
    bool enable_sqw_on_vbat;    /**< true: permite onda cuadrada 1 Hz respaldada por batería */
    bool enable_alarm1_int;     /**< true: habilita disparo físico en INT/SQW por Alarma 1 */
    bool enable_alarm2_int;     /**< true: habilita disparo físico en INT/SQW por Alarma 2 */
} ds3231m_config_t;

/* =========================================================================
 * Prototipos de funciones públicas
 * ========================================================================= */

/**
 * @brief Registra el DS3231M en el bus I2C maestro ya inicializado.
 *
 * @param[in] bus_handle Handle del bus I2C1 creado previamente.
 * @return ESP_OK si se agregó correctamente, o código de error ESP-IDF.
 */
esp_err_t ds3231m_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Configura los registros de Control (0x0E) y Estado (0x0F) del DS3231M.
 *
 * @param[in] cfg Puntero a la estructura de configuración operativa.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_config(const ds3231m_config_t *cfg);

/**
 * @brief Establece la fecha y hora actual del RTC (y limpia el flag OSF).
 *
 * @param[in] time Puntero a la estructura con fecha y hora válida en formato 24h.
 * @return ESP_OK en caso de éxito, ESP_ERR_INVALID_ARG si algún campo está fuera de rango.
 */
esp_err_t ds3231m_set_time(const ds3231m_time_t *time);

/**
 * @brief Lee la fecha y hora actual desde los registros del RTC en una ráfaga I2C.
 *
 * @param[out] time Puntero donde se almacenará la fecha y hora leída.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_get_time(ds3231m_time_t *time);

/**
 * @brief Verifica el estado del Oscillator Stop Flag (OSF) para saber si la hora es confiable.
 *
 * @param[out] is_stopped true si el oscilador se detuvo (hora inválida), false si corrió sin cortes.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_is_oscillator_stopped(bool *is_stopped);

/**
 * @brief Limpia manualmente la bandera Oscillator Stop Flag (OSF) en el registro 0x0F.
 *
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_clear_osf(void);

/**
 * @brief Lee la temperatura interna del sensor compensado del DS3231M (resolución 0.25 °C).
 *
 * @param[out] temp_c Puntero donde se almacenará la temperatura en grados Celsius.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_get_temperature(float *temp_c);

/**
 * @brief Programa los registros de la Alarma 1 (0x07 - 0x0A).
 *
 * @param[in] alarm Puntero a los parámetros de la Alarma 1.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_set_alarm1(const ds3231m_alarm1_t *alarm);

/**
 * @brief Programa los registros de la Alarma 2 (0x0B - 0x0D).
 *
 * @param[in] alarm Puntero a los parámetros de la Alarma 2.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_set_alarm2(const ds3231m_alarm2_t *alarm);

/**
 * @brief Consulta y limpia las banderas de disparo de Alarma 1 (A1F) y Alarma 2 (A2F).
 *
 * @param[out] alarm1_triggered true si disparó Alarma 1 (puede ser NULL si no interesa).
 * @param[out] alarm2_triggered true si disparó Alarma 2 (puede ser NULL si no interesa).
 * @param[in]  clear_flags      true para limpiar las banderas activas en el registro 0x0F.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t ds3231m_check_and_clear_alarms(bool *alarm1_triggered, bool *alarm2_triggered, bool clear_flags);

#ifdef __cplusplus
}
#endif

#endif /* DS3231M_H */