#ifndef FOTA_SERVICE_H
#define FOTA_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Confirma la imagen de firmware en ejecución ante el bootloader,
 *        cancelando el mecanismo de rollback automático si estaba en estado pendiente.
 */
void fota_confirmar_app_si_valida(void);

/**
 * @brief Crea e inicia la tarea FreeRTOS del cliente TLS encargada de
 *        enviar telemetría periódica y procesar actualizaciones FOTA.
 * @return ESP_OK si la tarea fue creada con éxito, o código de error en caso de fallo.
 */
esp_err_t fota_service_iniciar(void);

/**
 * @brief Detiene y elimina la tarea del cliente TLS si se encuentra en ejecución.
 */
void fota_service_detener(void);

#ifdef __cplusplus
}
#endif

#endif // FOTA_SERVICE_H
