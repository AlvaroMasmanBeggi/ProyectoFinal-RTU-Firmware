#include "fota_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#include "app_config.h"

static const char *TAG = "FOTA_SERVICE";

/* Activar en 1 para validar el certificado del servidor mediante ota_server_ca.pem */
#define FOTA_VERIFY_TLS_SERVER  0

#define FOTA_MAX_IMAGE_SIZE     (1024 * 1024)   // 1 MB límite por imagen
#define FOTA_LINE_SIZE          192
#define FOTA_CHUNK_BUF_SIZE     1024

#if FOTA_VERIFY_TLS_SERVER
extern const uint8_t ota_server_ca_pem_start[] asm("_binary_ota_server_ca_pem_start");
extern const uint8_t ota_server_ca_pem_end[]   asm("_binary_ota_server_ca_pem_end");
#endif

static TaskHandle_t s_tls_task_handle = NULL;

/* --- Funciones auxiliares de comunicación por socket TLS --- */

static bool tls_send_all(esp_tls_t *tls, const char *data, size_t length){
    size_t sent = 0;
    while (sent < length) {
        int result = esp_tls_conn_write(tls, data + sent, length - sent);
        if (result <= 0) {
            ESP_LOGE(TAG, "Error enviando datos por TLS: %d", result);
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

static bool tls_read_line(esp_tls_t *tls, char *line, size_t capacity){
    size_t length = 0;
    while (length + 1 < capacity) {
        int result = esp_tls_conn_read(tls, line + length, 1);
        if (result <= 0) {
            ESP_LOGE(TAG, "Error o cierre leyendo respuesta TLS: %d", result);
            return false;
        }
        if (line[length++] == '\n') {
            line[length] = '\0';
            return true;
        }
    }
    ESP_LOGE(TAG, "La respuesta del servidor excede la capacidad del buffer.");
    return false;
}

static bool convertir_sha256(const char *hex, uint8_t digest[32]){
    if (strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)hex[i * 2]) ||
            !isxdigit((unsigned char)hex[i * 2 + 1])) {
            return false;
        }
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        digest[i] = (uint8_t)byte;
    }
    return true;
}

/* --- Motor de descarga, cálculo de hash y escritura en flash --- */

static bool recibir_actualizacion_firmware(esp_tls_t *tls, const char *line){
    char version[32] = {0};
    char image_size_text[11] = {0};
    char sha256_hex[65] = {0};
    int consumed = 0;

    if (sscanf(line, "OTA %31s %10s %64s%n",
               version, image_size_text, sha256_hex, &consumed) != 3 ||
        line[consumed] != '\n' || line[consumed + 1] != '\0' ||
        image_size_text[0] == '\0') {
        ESP_LOGE(TAG, "Cabecera OTA inválida: %s", line);
        return false;
    }

    for (size_t i = 0; image_size_text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)image_size_text[i])) {
            ESP_LOGE(TAG, "Tamaño de imagen OTA con caracteres no numéricos.");
            return false;
        }
    }

    unsigned long long parsed_size = strtoull(image_size_text, NULL, 10);
    if (parsed_size == 0 || parsed_size > FOTA_MAX_IMAGE_SIZE) {
        ESP_LOGE(TAG, "Tamaño de imagen OTA fuera de rango (%llu bytes).", parsed_size);
        return false;
    }
    uint32_t image_size = (uint32_t)parsed_size;

    uint8_t expected_digest[32];
    if (!convertir_sha256(sha256_hex, expected_digest)) {
        ESP_LOGE(TAG, "SHA-256 recibido inválido.");
        return false;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || image_size > partition->size) {
        ESP_LOGE(TAG, "La imagen OTA (%u bytes) supera el espacio de la partición.", image_size);
        return false;
    }

    ESP_LOGI(TAG, "Iniciando recepción de versión '%s' (%u bytes) en partición '%s'...",
             version, image_size, partition->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, image_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar OTA en flash: %s", esp_err_to_name(err));
        return false;
    }

    // Inicializar cálculo de hash SHA-256 en streaming por hardware/software
    psa_hash_operation_t sha_operation = PSA_HASH_OPERATION_INIT;
    bool success = (psa_crypto_init() == PSA_SUCCESS) &&
                   (psa_hash_setup(&sha_operation, PSA_ALG_SHA_256) == PSA_SUCCESS);

    uint8_t buffer[FOTA_CHUNK_BUF_SIZE];
    uint32_t received = 0;

    while (success && received < image_size) {
        size_t remaining = image_size - received;
        size_t to_read = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int result = esp_tls_conn_read(tls, buffer, to_read);
        if (result <= 0) {
            ESP_LOGE(TAG, "Conexión interrumpida durante la descarga TLS: %d", result);
            success = false;
            break;
        }

        err = esp_ota_write(ota_handle, buffer, (size_t)result);
        if (err != ESP_OK ||
            psa_hash_update(&sha_operation, buffer, (size_t)result) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Error escribiendo en flash o actualizando hash SHA-256.");
            success = false;
            break;
        }
        received += (uint32_t)result;
    }

    uint8_t actual_digest[32];
    size_t actual_digest_size = 0;
    if (success &&
        (psa_hash_finish(&sha_operation, actual_digest, sizeof(actual_digest),
                         &actual_digest_size) != PSA_SUCCESS ||
         actual_digest_size != sizeof(actual_digest))) {
        success = false;
    }

    if (success && memcmp(expected_digest, actual_digest, sizeof(actual_digest)) != 0) {
        ESP_LOGE(TAG, "Discrepancia crítica: El SHA-256 calculado no coincide con el anunciado.");
        success = false;
    }

    if (!success) {
        psa_hash_abort(&sha_operation);
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "La estructura del binario OTA no es válida: %s", esp_err_to_name(err));
        return false;
    }

    esp_app_desc_t new_app;
    err = esp_ota_get_partition_description(partition, &new_app);
    if (err != ESP_OK || strncmp(version, new_app.version, sizeof(new_app.version)) != 0) {
        ESP_LOGE(TAG, "La versión declarada en cabecera (%s) difiere de la app descrita (%s).",
                 version, new_app.version);
        return false;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error seleccionando nueva partición de arranque: %s", esp_err_to_name(err));
        return false;
    }

    char acknowledgment[48];
    int length = snprintf(acknowledgment, sizeof(acknowledgment), "OK %s\n", version);
    if (length > 0 && (size_t)length < sizeof(acknowledgment)) {
        tls_send_all(tls, acknowledgment, (size_t)length);
    }

    ESP_LOGI(TAG, "FOTA exitosa. Reiniciando equipo para ejecutar versión %s...", version);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return true;
}

/* --- Tarea principal del cliente TLS --- */

static void tls_client_task(void *pvParameters){
    char payload[192];
    const esp_app_desc_t *app = esp_app_get_description();

    for (;;) {
        app_config_t config;
        if (!app_config_cargar(&config) || !app_config_validar(&config)) {
            ESP_LOGW(TAG, "Configuración no disponible o inválida. Reintentando en 5 s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        esp_tls_cfg_t cfg = {
            .timeout_ms = 10000,
        #if FOTA_VERIFY_TLS_SERVER
            .cacert_buf = ota_server_ca_pem_start,
            .cacert_bytes = ota_server_ca_pem_end - ota_server_ca_pem_start,
            .skip_common_name = false,
        #else
            .skip_common_name = true,
        #endif
        };

        esp_tls_t *tls = esp_tls_init();
        if (!tls) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Conectando TLS a %s:%d...", config.host_ip, config.tcp_port);
        int ret = esp_tls_conn_new_sync(config.host_ip, strlen(config.host_ip),
                                        config.tcp_port, &cfg, tls);

        if (ret == 1) {
            ESP_LOGI(TAG, "Conexión TLS establecida. Versión local: %s", app->version);

            while (true) {
                int length = snprintf(payload, sizeof(payload),
                    "{\"sensor_id\":\"SENS-01\",\"fecha\":\"2026-10-15\","
                    "\"hora\":\"12:00:00\",\"firmware_version\":\"%s\"}\n",
                    app->version);

                if (length <= 0 || (size_t)length >= sizeof(payload) ||
                    !tls_send_all(tls, payload, (size_t)length)) {
                    break;
                }

                char response[FOTA_LINE_SIZE];
                if (!tls_read_line(tls, response, sizeof(response))) {
                    break;
                }

                if (strcmp(response, "NONE\n") == 0) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }

                if (strncmp(response, "OTA ", 4) == 0) {
                    recibir_actualizacion_firmware(tls, response);
                    break;
                }

                ESP_LOGE(TAG, "Respuesta no reconocida del servidor: %s", response);
                break;
            }
        }

        esp_tls_conn_destroy(tls);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* --- API Pública expuesta por el encabezado --- */

void fota_confirmar_app_si_valida(void){
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Confirmando validez de partición '%s' (Rollback cancelado).", running->label);
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
}

esp_err_t fota_service_iniciar(void){
    if (s_tls_task_handle != NULL) {
        ESP_LOGW(TAG, "La tarea FOTA TLS ya se encuentra en ejecución.");
        return ESP_OK;
    }

    BaseType_t res = xTaskCreate(tls_client_task, "tls_client_task", 8192, NULL, 5, &s_tls_task_handle);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Error al crear la tarea tls_client_task.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void fota_service_detener(void){
    if (s_tls_task_handle != NULL) {
        vTaskDelete(s_tls_task_handle);
        s_tls_task_handle = NULL;
        ESP_LOGI(TAG, "Tarea FOTA TLS detenida.");
    }
}
