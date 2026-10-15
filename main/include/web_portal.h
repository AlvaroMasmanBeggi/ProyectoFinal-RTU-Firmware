#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicia el servidor HTTP local para el portal cautivo de configuración
 *        y registra los endpoints GET / y POST /guardar.
 * @return ESP_OK si inició correctamente, o código de error en caso de fallo.
 */
esp_err_t web_portal_iniciar(void);

/**
 * @brief Detiene el servidor HTTP si está activo y libera sus recursos.
 */
void web_portal_detener(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_PORTAL_H
