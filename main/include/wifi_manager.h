#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MAX_SCAN_REDES    15
#define WIFI_MAX_REINTENTOS    5

/* Credenciales del punto de acceso local (Portal Cautivo) */
#define WIFI_AP_SSID           "RTU_AGC"
#define WIFI_AP_PASS           "agroconnect"
#define WIFI_AP_MAX_CONN       4

/**
 * @brief Inicializa la pila de red, el loop de eventos por defecto
 *        y las interfaces netif para estación (STA) y punto de acceso (AP).
 * @return ESP_OK si la inicialización fue exitosa.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Intenta conectar al Wi-Fi como estación (STA) con los parámetros dados.
 *        Bloquea la tarea llamante hasta lograr conexión con IP o agotar los reintentos.
 * @param[in] config Puntero a la configuración que contiene SSID y contraseña.
 * @return true si se conectó y obtuvo dirección IP, false si agotó los reintentos.
 */
bool wifi_manager_conectar_sta(const app_config_t *config);

/**
 * @brief Levanta el punto de acceso (AP) propio para configuración,
 *        realiza un escaneo de redes circundantes y arranca el portal web.
 */
void wifi_manager_iniciar_ap(void);

/**
 * @brief Realiza un escaneo síncrono del espectro RF en busca de puntos de acceso cercanos.
 */
void wifi_manager_escanear_redes(void);

/**
 * @brief Devuelve la lista y cantidad de redes detectadas en el último escaneo.
 * @param[out] redes Puntero doble que apuntará al arreglo interno de redes.
 * @return Cantidad de redes registradas (hasta WIFI_MAX_SCAN_REDES).
 */
uint16_t wifi_manager_get_redes_escaneadas(const wifi_ap_record_t **redes);

/**
 * @brief Indica si el equipo está actualmente en modo portal de configuración.
 * @return true si el AP está activo y en configuración, false en operación normal STA.
 */
bool wifi_manager_en_modo_configuracion(void);

/**
 * @brief Fuerza la desconexión de la red actual (utilizado ante comandos por UART).
 */
void wifi_manager_desconectar(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
