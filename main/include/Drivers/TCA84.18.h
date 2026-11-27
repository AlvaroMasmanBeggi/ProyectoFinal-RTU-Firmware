/**
 * @file TCA8418.h
 * @brief Driver I2C para el controlador de matriz de teclado TCA8418 (ESP-IDF i2c_master).
 *
 * Soporta matrices de hasta 8 filas x 10 columnas (80 teclas), memoria FIFO
 * interna de 10 eventos (presión/liberación), filtrado antirrebote por hardware,
 * bloqueo/desbloqueo de teclado por secuencia y manejo de GPIOs auxiliares.
 */

#ifndef TCA8418_H
#define TCA8418_H

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
 * Configuración del bus I2C1 y sincronización
 * ========================================================================= */
#define TCA8418_I2C_ADDR            0x34    /**< Dirección I2C fija de 7 bits (0b0110100) */
#define TCA8418_I2C_FREQ_HZ         400000  /**< Frecuencia Fast-Mode I2C (400 kHz) */
#define TCA8418_FIFO_MAX_SIZE       10      /**< Capacidad máxima de la FIFO interna */

#ifndef USE_MUTEX
#define USE_MUTEX                   1
#endif

#ifndef I2C_MUTEX_TIMEOUT_MS
#define I2C_MUTEX_TIMEOUT_MS        100
#endif

/* Handles globales definidos en app_main.c y compartidos con los drivers */
extern i2c_master_dev_handle_t TCA8418_dev;
extern SemaphoreHandle_t I2C1_Mutex;

/* =========================================================================
 * Mapa de registros internos del TCA8418 (Tabla 9 del datasheet)
 * Nota: Los registros 0x00 y 0x2F son reservados y no deben direccionarse.
 * ========================================================================= */
#define TCA8418_REG_CFG             0x01    /**< Registro de configuración general e interrupciones */
#define TCA8418_REG_INT_STAT        0x02    /**< Estado de banderas de interrupción (W1C) */
#define TCA8418_REG_KEY_LCK_EC      0x03    /**< Control de bloqueo y contador de eventos en FIFO */
#define TCA8418_REG_KEY_EVENT_A     0x04    /**< Cabeza de la FIFO de eventos (lectura extrae evento) */
#define TCA8418_REG_KEY_EVENT_B     0x05
#define TCA8418_REG_KEY_EVENT_C     0x06
#define TCA8418_REG_KEY_EVENT_D     0x07
#define TCA8418_REG_KEY_EVENT_E     0x08
#define TCA8418_REG_KEY_EVENT_F     0x09
#define TCA8418_REG_KEY_EVENT_G     0x0A
#define TCA8418_REG_KEY_EVENT_H     0x0B
#define TCA8418_REG_KEY_EVENT_I     0x0C
#define TCA8418_REG_KEY_EVENT_J     0x0D    /**< Cola de la FIFO de eventos */
#define TCA8418_REG_KP_LCK_TIMER    0x0E    /**< Temporizadores de secuencia de desbloqueo */
#define TCA8418_REG_UNLOCK1         0x0F    /**< Código de primera tecla de desbloqueo */
#define TCA8418_REG_UNLOCK2         0x10    /**< Código de segunda tecla de desbloqueo */

/* Registros de estado, datos y dirección de puertos (1: ROW0-7, 2: COL0-7, 3: COL8-9) */
#define TCA8418_REG_GPIO_INT_STAT1  0x11
#define TCA8418_REG_GPIO_INT_STAT2  0x12
#define TCA8418_REG_GPIO_INT_STAT3  0x13
#define TCA8418_REG_GPIO_DAT_STAT1  0x14
#define TCA8418_REG_GPIO_DAT_STAT2  0x15
#define TCA8418_REG_GPIO_DAT_STAT3  0x16
#define TCA8418_REG_GPIO_DAT_OUT1   0x17
#define TCA8418_REG_GPIO_DAT_OUT2   0x18
#define TCA8418_REG_GPIO_DAT_OUT3   0x19
#define TCA8418_REG_GPIO_INT_EN1    0x1A
#define TCA8418_REG_GPIO_INT_EN2    0x1B
#define TCA8418_REG_GPIO_INT_EN3    0x1C
#define TCA8418_REG_KP_GPIO1        0x1D    /**< Selección Modo Teclado Matriz(1) / GPIO(0) para ROW0..ROW7 */
#define TCA8418_REG_KP_GPIO2        0x1E    /**< Selección Modo Teclado Matriz(1) / GPIO(0) para COL0..COL7 */
#define TCA8418_REG_KP_GPIO3        0x1F    /**< Selección Modo Teclado Matriz(1) / GPIO(0) para COL8..COL9 */
#define TCA8418_REG_GPI_EM1         0x20
#define TCA8418_REG_GPI_EM2         0x21
#define TCA8418_REG_GPI_EM3         0x22
#define TCA8418_REG_GPIO_DIR1       0x23    //GPIO data direction, 0: input, 1: output
#define TCA8418_REG_GPIO_DIR2       0x24
#define TCA8418_REG_GPIO_DIR3       0x25
#define TCA8418_REG_GPIO_INT_LVL1   0x26    //GPIO edge/level detection, 0: falling/low, 1: rising/high
#define TCA8418_REG_GPIO_INT_LVL2   0x27
#define TCA8418_REG_GPIO_INT_LVL3   0x28
#define TCA8418_REG_DEBOUNCE_DIS1   0x29    /**< 0: Debounce habilitado, 1: Deshabilitado (ROW0..7) */
#define TCA8418_REG_DEBOUNCE_DIS2   0x2A    /**< 0: Debounce habilitado, 1: Deshabilitado (COL0..7) */
#define TCA8418_REG_DEBOUNCE_DIS3   0x2B    /**< 0: Debounce habilitado, 1: Deshabilitado (COL8..9) */
#define TCA8418_REG_GPIO_PULL1      0x2C    /**< 0: Pull-up interno activo, 1: Deshabilitado (ROW0..7) */
#define TCA8418_REG_GPIO_PULL2      0x2D    /**< 0: Pull-up interno activo, 1: Deshabilitado (COL0..7) */
#define TCA8418_REG_GPIO_PULL3      0x2E    /**< 0: Pull-up interno activo, 1: Deshabilitado (COL8..9) */

/* =========================================================================
 * Máscaras de bits de registros principales
 * ========================================================================= */

/* Registro CFG (0x01) */
#define TCA8418_CFG_AI              (1U << 7) /**< 1: Autoincremento(habilitado) de dirección de registro en ráfagas */
#define TCA8418_CFG_GPI_E_CFG       (1U << 6) /**< 1: No rastrear eventos GPI cuando el teclado está bloqueado */
#define TCA8418_CFG_OVR_FLOW_M      (1U << 5) /**< 1: En overflow, desplaza FIFO descartando el evento más viejo */
#define TCA8418_CFG_INT_CFG         (1U << 4) /**< 1: Pulso alto de 50 us en INT si quedan eventos pendientes */
#define TCA8418_CFG_OVR_FLOW_IEN    (1U << 3) /**< 1: Habilita interrupción por desbordamiento de FIFO */
#define TCA8418_CFG_K_LCK_IEN       (1U << 2) /**< 1: Habilita interrupción por secuencia de desbloqueo */
#define TCA8418_CFG_GPI_IEN         (1U << 1) /**< 1: Habilita interrupción por cambio en pines GPI */
#define TCA8418_CFG_KE_IEN          (1U << 0) /**< 1: Habilita interrupción por evento de tecla (press/release) */

/* Registro INT_STAT (0x02) - Se limpian escribiendo '1' en el bit correspondiente */
#define TCA8418_INT_CAD             (1U << 4) /**< Secuencia Ctrl+Alt+Del (teclas 1+11+21) detectada */
#define TCA8418_INT_OVR_FLOW        (1U << 3) /**< Desbordamiento de la FIFO de 10 bytes */
#define TCA8418_INT_K_LCK           (1U << 2) /**< Evento de bloqueo/desbloqueo de teclado */
#define TCA8418_INT_GPI             (1U << 1) /**< Interrupción generada por pin GPI */
#define TCA8418_INT_KEY             (1U << 0) /**< Evento de tecla disponible en la FIFO */

/* Registro KEY_LCK_EC (0x03) */
#define TCA8418_LCK_EN              (1U << 6) /**< 1: Bloquea el teclado, 0: Desbloquea manualmente */
#define TCA8418_LCK_STATUS_MASK     (0x03U << 4) /**< Bits [5:4]: Estado de bloqueo porque el bit 4 y 5 los dos estan en 1(LCK2:LCK1)*/
#define TCA8418_FIFO_COUNT_MASK     0x0FU     /**< Bits [3:0]: Cantidad de eventos almacenados (0..10). Indica cuantas teclas presionadas/soltadas hay guardads esperando ser leidas */

/* Registro KEY_EVENT_A (0x04) */
#define TCA8418_EVENT_PRESS_MASK    (1U << 7) /**< Bit 7: 1 = Tecla presionada, 0 = Tecla liberada */
#define TCA8418_EVENT_KEY_MASK      0x7FU     /**< Bits [6:0]: Código de tecla (0..80) o GPI (97..114) */

/* =========================================================================
 * Tipos de datos y estructuras
 * ========================================================================= */

/**
 * @brief Representación decodificada de un evento extraído de la FIFO.
 */
typedef struct {
    uint8_t raw_byte;       /**< Byte crudo leído de KEY_EVENT_A (0x04) */
    uint8_t key_code;       /**< Código numérico de tecla (1..80 en matriz, 97..114 en GPI) */
    bool    is_pressed;     /**< true: tecla presionada (flanco activo), false: tecla liberada */
    bool    is_gpi_event;   /**< true: evento proveniente de GPI (97..114), false: matriz (1..80) */
    uint8_t row;            /**< Fila decodificada (0..7) válida cuando is_gpi_event == false */
    uint8_t col;            /**< Columna decodificada (0..9) válida cuando is_gpi_event == false */
} tca8418_key_event_t;

/**
 * @brief Estado de las banderas de interrupción del registro INT_STAT (0x02).
 */
typedef struct {
    bool key_event;         /**< K_INT: Hay eventos de tecla en la FIFO */
    bool gpi_event;         /**< GPI_INT: Cambio de estado en entrada GPI */
    bool keypad_lock;       /**< K_LCK_INT: Se completó la secuencia de desbloqueo */
    bool overflow;          /**< OVR_FLOW_INT: La FIFO superó los 10 eventos */
    bool cad_sequence;      /**< CAD_INT: Se presionaron juntas las teclas 1, 11 y 21 */
} tca8418_int_status_t;

/**
 * @brief Estructura de configuración inicial del TCA8418.
 */
typedef struct {
    uint8_t  rows_mask;             /**< Máscara de filas en matriz (bit 0 = ROW0 .. bit 7 = ROW7) */
    uint16_t cols_mask;             /**< Máscara de columnas en matriz (bit 0 = COL0 .. bit 9 = COL9) */
    bool     enable_key_int;        /**< true: habilita interrupción INT por eventos de teclado */
    bool     enable_gpi_int;        /**< true: habilita interrupción INT por pines GPI */
    bool     enable_overflow_int;   /**< true: habilita interrupción INT si desborda la FIFO */
    bool     int_deassert_50us;     /**< true: pulso de 50 us en INT si quedan eventos pendientes */
    bool     overflow_wrap;         /**< true: sobrescribe evento más viejo en overflow; false: descarta nuevo */
    bool     enable_debounce;       /**< true: activa filtro antirrebote por hardware en entradas */
    bool     enable_pullups;        /**< true: activa resistencias pull-up internas (~100 kOhm) */
} tca8418_config_t;

/* =========================================================================
 * Prototipos de funciones públicas
 * ========================================================================= */

/**
 * @brief Registra el TCA8418 en el bus maestro I2C1 ya inicializado.
 *
 * @param[in] bus_handle Handle del bus I2C1 creado previamente en app_main.
 * @return ESP_OK si se registró correctamente, o código de error ESP-IDF.
 */
esp_err_t tca8418_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Configura la matriz de teclas, pull-ups, antirrebote e interrupciones.
 *
 * @param[in] cfg Puntero a la estructura de configuración operativa.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_config(const tca8418_config_t *cfg);

/**
 * @brief Consulta cuántos eventos de tecla están pendientes en la FIFO interna.
 *
 * @param[out] count Puntero donde se almacenará la cantidad de eventos (0..10).
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_get_event_count(uint8_t *count);

/**
 * @brief Extrae y decodifica el evento más antiguo de la FIFO (registro 0x04).
 *
 * Al leer KEY_EVENT_A (0x04), el TCA8418 desplaza automáticamente la FIFO
 * y decrementa el contador KEC[3:0].
 *
 * @param[out] event     Puntero donde se guardará el evento decodificado.
 * @param[out] has_event true si se leyó un evento válido, false si la FIFO estaba vacía.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_read_event(tca8418_key_event_t *event, bool *has_event);

/**
 * @brief Vacía por completo la FIFO de eventos y limpia las banderas pendientes.
 *
 * Útil durante el arranque para descartar pulsaciones espurias previas.
 *
 * @param[out] flushed_count Opcional (puede ser NULL): devuelve cuántos eventos se descartaron.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_flush_fifo(uint8_t *flushed_count);

/**
 * @brief Lee el registro de estado de interrupciones INT_STAT (0x02) y opcionalmente las limpia.
 *
 * @param[out] status      Puntero a la estructura donde se volcarán las banderas activas.
 * @param[in]  clear_flags true para escribir '1' en las banderas detectadas y limpiar INT_STAT.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_get_and_clear_int_status(tca8418_int_status_t *status, bool clear_flags);

/**
 * @brief Bloquea o desbloquea manualmente el teclado mediante el bit K_LCK_EN (0x03).
 *
 * @param[in] lock true para bloquear el teclado, false para desbloquearlo.
 * @return ESP_OK en caso de éxito, o código de error ESP-IDF.
 */
esp_err_t tca8418_set_keypad_lock(bool lock);

/**
 * @brief Configura las teclas y temporizadores para el desbloqueo por secuencia de 2 teclas.
 *
 * @param[in] key1_code        Código de la primera tecla de desbloqueo (1..80).
 * @param[in] key2_code        Código de la segunda tecla de desbloqueo (1..80).
 * @param[in] unlock_timeout_s Tiempo máximo entre tecla 1 y tecla 2 (1..7 segundos).
 * @param[in] mask_timeout_s   Tiempo de máscara de interrupciones (0..31 segundos, 0 = desactivado).
 * @return ESP_OK en caso de éxito, ESP_ERR_INVALID_ARG si algún parámetro excede el rango.
 */
esp_err_t tca8418_configure_unlock_sequence(uint8_t key1_code, uint8_t key2_code,
                                            uint8_t unlock_timeout_s, uint8_t mask_timeout_s);

#ifdef __cplusplus
}
#endif

#endif /* TCA8418_H */