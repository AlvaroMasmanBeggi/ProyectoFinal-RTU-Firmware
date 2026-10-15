#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Límites de tamaño de buffers (incluyen el byte nulo '\0') --- */
#define CONFIG_SSID_MAX_LEN        33   // 32 caracteres + '\0'
#define CONFIG_PASS_MAX_LEN        65   // 64 caracteres + '\0'
#define CONFIG_HOST_IP_MAX_LEN     16   // Formato "xxx.xxx.xxx.xxx" + '\0'

/* --- Valores por defecto --- */
#define CONFIG_DEFAULT_TCP_PORT    3334
#define CONFIG_DEFAULT_UDP_PORT    5000

/**
 * @brief Estructura que agrupa todos los parámetros de conexión y red de la RTU.
 */
typedef struct {
    char ssid[CONFIG_SSID_MAX_LEN];
    char pass[CONFIG_PASS_MAX_LEN];
    char host_ip[CONFIG_HOST_IP_MAX_LEN];
    int tcp_port;
    int udp_port;
} app_config_t;

/**
 * @brief Inicializa la memoria Flash NVS.
 *        Si detecta partición sin páginas libres o cambio de versión, realiza borrado preventivo.
 * @return ESP_OK si se inicializó correctamente, o el código de error de ESP-IDF.
 */
esp_err_t app_config_init_nvs(void);

/**
 * @brief Carga las credenciales y puertos almacenados en la partición NVS.
 * @param[out] config Puntero a la estructura donde se copiarán los datos leídos.
 * @return true si se encontró al menos el SSID en NVS, false si está vacío o hubo error.
 */
bool app_config_cargar(app_config_t *config);

/**
 * @brief Guarda y confirma (commit) los nuevos parámetros en NVS.
 * @param[in] config Puntero a la estructura con los datos a persistir.
 * @return ESP_OK si la escritura fue exitosa, o código de error en caso de fallo.
 */
esp_err_t app_config_guardar(const app_config_t *config);

/**
 * @brief Comprueba que los parámetros en memoria sean válidos (SSID no vacío,
 *        formato de IPv4 correcto y puertos dentro del rango 1 - 65535).
 * @param[in] config Puntero a la estructura a evaluar.
 * @return true si todos los campos son válidos, false si alguno presenta formato erróneo.
 */
bool app_config_validar(const app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // APP_CONFIG_H
