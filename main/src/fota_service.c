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
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#include "app_config.h"

static const char *TAG = "FOTA_SERVICE";

#define FOTA_MAX_IMAGE_SIZE     (1024 * 1024)   // 1 MB límite por imagen
#define FOTA_LINE_SIZE          192
#define FOTA_CHUNK_BUF_SIZE     1024

// El archivo PEM se incorpora al firmware mediante EMBED_TXTFILES.
// start apunta al primer byte y end al primer byte posterior al certificado.
extern const uint8_t ota_server_ca_pem_start[] asm("_binary_ota_server_ca_pem_start");
extern const uint8_t ota_server_ca_pem_end[]   asm("_binary_ota_server_ca_pem_end");

static TaskHandle_t s_tls_task_handle = NULL;

/* --- Funciones auxiliares de comunicación por socket TLS --- */

static bool tls_send_all(esp_tls_t *tls, const char *data, size_t length){
    size_t sent = 0;
    // Un envío TLS puede transferir menos bytes que los solicitados.
    // Se continúa hasta transmitir todo el mensaje o detectar un error.
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

//esta funcion convierte un hash SHA-256 en formato hexadecimal a un arreglo de bytes de 32 elementos
static bool convertir_sha256(const char *hex, uint8_t digest[32]){
    // El servidor anuncia un SHA-256 como 64 caracteres hexadecimales.
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

//esta funcion recibe la respuesta del servidor con la cabecera OTA y procede a descargar la imagen de firmware, calcular su hash SHA-256 y escribirla en la partición OTA correspondiente
static bool recibir_actualizacion_firmware(esp_tls_t *tls, const char *line){
    // Campos contenidos en la respuesta "OTA <version> <tamano> <sha256>".
    char version[32] = {0};
    char image_size_text[11] = {0};
    char sha256_hex[65] = {0};
    int consumed = 0;

    // Extrae la cabecera OTA y verifica que termine después de los tres campos.
    if (sscanf(line, "OTA %31s %10s %64s%n",
               version, image_size_text, sha256_hex, &consumed) != 3 ||
        line[consumed] != '\n' || line[consumed + 1] != '\0' ||
        image_size_text[0] == '\0') {
        ESP_LOGE(TAG, "Cabecera OTA inválida: %s", line);
        return false;
    }

    // Verifica que el tamaño de imagen sea un número válido y esté dentro del límite.
    for (size_t i = 0; image_size_text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)image_size_text[i])) {
            ESP_LOGE(TAG, "Tamaño de imagen OTA con caracteres no numéricos.");
            return false;
        }
    }

    // Convierte el tamaño de imagen de texto a entero sin signo.
    unsigned long long parsed_size = strtoull(image_size_text, NULL, 10);
    if (parsed_size == 0 || parsed_size > FOTA_MAX_IMAGE_SIZE) {
        ESP_LOGE(TAG, "Tamaño de imagen OTA fuera de rango (%llu bytes).", parsed_size);
        return false;
    }
    uint32_t image_size = (uint32_t)parsed_size;

    // Convierte el SHA-256 recibido de texto hexadecimal a 32 bytes.
    uint8_t expected_digest[32];
    if (!convertir_sha256(sha256_hex, expected_digest)) {
        ESP_LOGE(TAG, "SHA-256 recibido inválido.");
        return false;
    }

    // Selecciona el slot OTA que no está ejecutándose actualmente.
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || image_size > partition->size) {
        ESP_LOGE(TAG, "La imagen OTA (%u bytes) supera el espacio de la partición.", image_size);
        return false;
    }

    // La respuesta OTA ya fue validada; confirma la imagen actual antes de instalar otra.
    fota_confirmar_app_si_valida();

    ESP_LOGI(TAG, "Iniciando recepción de versión '%s' (%u bytes) en partición '%s'...",
             version, image_size, partition->label);

    // Inicia una operación OTA; ota_handle identifica esta escritura en flash.
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

    // Descarga la imagen por bloques, escribiendo cada bloque en el slot OTA
    // y agregándolo simultáneamente al cálculo incremental del SHA-256.
    uint8_t buffer[FOTA_CHUNK_BUF_SIZE];
    uint32_t received = 0;

    // Lee hasta que se reciba toda la imagen o ocurra un error.
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

    // Finaliza el cálculo SHA-256 y exige exactamente 32 bytes de resultado.
    uint8_t actual_digest[32];
    size_t actual_digest_size = 0;
    if (success &&
        (psa_hash_finish(&sha_operation, actual_digest, sizeof(actual_digest),
                         &actual_digest_size) != PSA_SUCCESS ||
         actual_digest_size != sizeof(actual_digest))) {
        success = false;
    }

    // Compara el SHA-256 calculado con el anunciado por el servidor.
    if (success && memcmp(expected_digest, actual_digest, sizeof(actual_digest)) != 0) {
        ESP_LOGE(TAG, "Discrepancia crítica: El SHA-256 calculado no coincide con el anunciado.");
        success = false;
    }

    // Ante cualquier fallo se aborta la escritura y no se modifica la partición activa.
    if (!success) {
        psa_hash_abort(&sha_operation);
        esp_ota_abort(ota_handle);
        return false;
    }

    // Cierra y valida la operación OTA. Solo después se permite el arranque alternativo.
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "La estructura del binario OTA no es válida: %s", esp_err_to_name(err));
        return false;
    }

    // Valida que la versión escrita dentro del binario coincida con la anunciada por el servidor.
    esp_app_desc_t new_app;
    err = esp_ota_get_partition_description(partition, &new_app);
    if (err != ESP_OK || strncmp(version, new_app.version, sizeof(new_app.version)) != 0) {
        ESP_LOGE(TAG, "La versión declarada en cabecera (%s) difiere de la app descrita (%s).",
                 version, new_app.version);
        return false;
    }

    // Selecciona la partición OTA recién escrita como la próxima a arrancar.
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error seleccionando nueva partición de arranque: %s", esp_err_to_name(err));
        return false;
    }

    // Confirma exitosamente al servidor antes de reiniciar hacia la nueva imagen.
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
    char device_id[19];
    // La descripción contiene la versión compilada en el firmware actual.
    const esp_app_desc_t *app = esp_app_get_description();

    for (;;) {
        // Cada ciclo vuelve a cargar la configuración; así también admite cambios dinámicos.
        app_config_t config;
        if (!app_config_cargar(&config) || !app_config_validar(&config)) {
            ESP_LOGW(TAG, "Configuración no disponible o inválida. Reintentando en 5 s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Obtiene la dirección MAC de la interfaz Wi-Fi para generar un identificador único.
        uint8_t mac[6];
        esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);
        if (mac_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo obtener el MAC para identificar el dispositivo: %s",
                     esp_err_to_name(mac_err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue; 
        }
        // Identificador enviado al servidor para diferenciar esta RTU de otras.
        snprintf(device_id, sizeof(device_id), "ESP32-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Configura la autenticación TLS con el certificado CA incluido en el firmware.
        esp_tls_cfg_t cfg = {
            .timeout_ms = 10000,
            .cacert_buf = ota_server_ca_pem_start,
            .cacert_bytes = ota_server_ca_pem_end - ota_server_ca_pem_start,
            // El certificado de prueba está emitido para localhost y la RTU conecta por IP.
            .skip_common_name = true,
        };

        // Inicializa la estructura de conexión TLS y establece la conexión con el servidor.
        // Si falla, espera 2 segundos y reintenta la conexión.
        //esp_tls_init() devuelve un puntero a esp_tls_t que representa la conexión TLS.Es como un descriptor de socket seguro.
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

            // Tras conectarse, identifica el dispositivo y espera una respuesta del servidor.
            while (true) {
                int length = snprintf(payload, sizeof(payload),
                    "{\"sensor_id\":\"%s\",\"firmware_version\":\"%s\"}\n",
                    device_id, app->version);

                //verifica que la longitud del mensaje no exceda el tamaño del buffer y que se haya enviado correctamente.
                if (length <= 0 || (size_t)length >= sizeof(payload) ||
                    !tls_send_all(tls, payload, (size_t)length)) {
                    break;
                }
                
                //lee la respuesta del servidor, que puede ser "NONE\n" o "OTA <version> <tamano> <sha256>\n"
                char response[FOTA_LINE_SIZE];
                if (!tls_read_line(tls, response, sizeof(response))) {
                    break;
                }

                // Si el servidor no tiene actualizaciones, espera 5 segundos antes de volver a enviar telemetría.
                if (strcmp(response, "NONE\n") == 0) {
                    fota_confirmar_app_si_valida();
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }

                // OTA recibe además el tamaño y SHA-256; deriva a la rutina de instalación.
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
    // Después de arrancar una nueva imagen, esta confirma que es válida.
    // Sin esta llamada el bootloader puede ejecutar rollback tras el siguiente reinicio.
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
