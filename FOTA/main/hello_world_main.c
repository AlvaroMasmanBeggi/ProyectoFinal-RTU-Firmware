#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h" // NUEVA LIBRERÍA: Para operaciones directas con NVS
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include <esp_http_server.h>
#include <ctype.h>
#include "psa/crypto.h"

// Parámetros de actualización remota. Activar la verificación TLS antes de producción.
#define FOTA_VERIFY_TLS_SERVER 0
// Límite adicional al tamaño del slot OTA disponible en la tabla de particiones.
#define FOTA_MAX_IMAGE_SIZE (1024 * 1024)
// Capacidad del buffer para líneas de control del protocolo FOTA.
#define FOTA_LINE_SIZE 192

// Símbolos generados por CMake al incrustar ota_server_ca.pem en el firmware.
extern const uint8_t ota_server_ca_pem_start[] asm("_binary_ota_server_ca_pem_start");
extern const uint8_t ota_server_ca_pem_end[] asm("_binary_ota_server_ca_pem_end");

// Redes encontradas en el último escaneo, que se presentan en el portal AP.
#define MAX_REDES 15
wifi_ap_record_t redes_encontradas[MAX_REDES];
uint16_t cantidad_redes = 0;

// --- CONFIGURACIONES GLOBALES ---
#define UART_PORT_NUM  UART_NUM_0 //Puerto UART para recibir comandos serial(config)
#define BUF_SIZE       1024 // Tamaño del buffer de recepción UART
#define MAX_REINTENTOS 5 // Límite antes de rendirse y levantar el AP
#define MAX_FORM_BODY_SIZE 512

static EventGroupHandle_t s_wifi_event_group; //Se utiliza para esperar a que la conexión Wi-Fi se establezca (o falle). Es sincronización.
#define WIFI_CONNECTED_BIT BIT0 //si la conexion es exitosa, se establece este bit
#define WIFI_FAIL_BIT      BIT1 //si falla la conexion, se establece este bit
                                //el main thread espera a que uno de estos bits se establezca para continuar
static const char *TAG = "RTU_CORE";
static int s_retry_num = 0;
// Evita reconectar como estación mientras el usuario configura el equipo.
bool modo_configuracion = false;

// Copias en RAM de los parámetros persistidos en NVS.
char rtu_ssid[33] = "";
char rtu_pass[65] = "";
char rtu_host_ip[16] = "";
int rtu_tcp_port = 3334;
int rtu_udp_port = 5000;

// Prototipos de las tareas de red, actualización y configuración.
static void tls_client_task(void *pvParameters);
static void uart_monitor_task(void *pvParameters);
static bool tls_send_all(esp_tls_t *tls, const char *data, size_t length);
static bool tls_read_line(esp_tls_t *tls, char *line, size_t capacity);
static bool recibir_actualizacion_firmware(esp_tls_t *tls, const char *line);
static void confirmar_app_si_valida(void);
void iniciar_modo_ap(void);
static httpd_handle_t iniciar_servidor_web(void);

// -----------------------------------------------------------------
// LECTURA / ESCRITURA EN MEMORIA NO VOLÁTIL (NVS)
// -----------------------------------------------------------------
// Lee de NVS las credenciales Wi-Fi y la dirección/puertos del servidor.
// Devuelve false si no existe la configuración mínima (el SSID).
bool leer_credenciales_nvs(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle); //abre un espacio de almacenamiento llamado "storage" en modo lectura
    if (err != ESP_OK) return false;

    size_t size = sizeof(rtu_ssid);
    err = nvs_get_str(my_handle, "ssid", rtu_ssid, &size); //lee la clave "ssid" y la guarda en rtu_ssid
    if (err != ESP_OK) {
        nvs_close(my_handle); //si no la encuentra, cierra el handle y retorna false
        return false;
    }

    size = sizeof(rtu_pass);
    nvs_get_str(my_handle, "pass", rtu_pass, &size);

    size = sizeof(rtu_host_ip);
    nvs_get_str(my_handle, "host_ip", rtu_host_ip, &size);

    nvs_get_i32(my_handle, "tcp_port", (int32_t *)&rtu_tcp_port);
    nvs_get_i32(my_handle, "udp_port", (int32_t *)&rtu_udp_port);

    nvs_close(my_handle);
    return true;
}


// Persiste los valores recibidos por el portal para recuperarlos tras reiniciar.
void guardar_credenciales_nvs(const char* ssid, const char* pass, const char* host_ip, int tcp, int udp) {
    nvs_handle_t my_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "pass", pass));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "host_ip", host_ip));
    ESP_ERROR_CHECK(nvs_set_i32(my_handle, "tcp_port", tcp));
    ESP_ERROR_CHECK(nvs_set_i32(my_handle, "udp_port", udp));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Credenciales guardadas con éxito en NVS.");
}

// -----------------------------------------------------------------
// HANDLER DE EVENTOS DE RED (WI-FI E IP)
// -----------------------------------------------------------------
// ESP-IDF invoca esta función cuando cambia el estado de Wi-Fi o se obtiene una IP.
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Solo intentamos conectar automáticamente si NO estamos configurando
        if (!modo_configuracion) {
            esp_wifi_connect(); //se conecta cuando la estacion wifi empieza y si no estamos configurando, porque la antena es una jaja
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!modo_configuracion && s_retry_num < MAX_REINTENTOS) {
            esp_wifi_connect(); //si se desconectó y no estamos configurando, intentamos reconectar
            s_retry_num++;
            ESP_LOGI(TAG, "Reintentando conexión al router (%d/%d)...", s_retry_num, MAX_REINTENTOS);
        } else if (!modo_configuracion) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT); //si superamos el maximo de reintentos, establecemos el bit de fallo
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); //si conecto y obtuvo IP, establecemos el bit de conexión
    }
}

//Esto genera una lógica robusta:

//intenta conectarse
//si falla, reintenta varias veces
//si no logra conectar, activa modo AP y pide configurar la Wi‑Fi

// -----------------------------------------------------------------
// MAIN: PUNTO DE ENTRADA DEL PROGRAMA
// -----------------------------------------------------------------
// Punto de entrada: prepara memoria/red, conecta usando NVS o abre el portal AP.
void app_main(void)
{
    // 1. Inicializar NVS, donde se guarda la configuración entre reinicios.
    // Si sus páginas no son válidas o la versión cambió, se inicializa de nuevo.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase()); // Borra la memoria NVS si está corrupta o desactualizada
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Preparar interfaces Wi-Fi, pila IP y despacho de eventos.
    s_wifi_event_group = xEventGroupCreate(); // Crea el grupo de eventos para sincronizar la conexión Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init()); // Inicializa la pila de red TCP/IP
    ESP_ERROR_CHECK(esp_event_loop_create_default()); //Esto es para que el event_handler funcione, crea un loop de eventos por defecto. 
                                                      //Un loop de eventos es como un "bucle" que escucha y maneja eventos de red de manera asíncrona.
    esp_netif_create_default_wifi_sta(); // Crea la interfaz de red por defecto para el modo estación (STA)
                                         // La interfaz de red es como un "canal" que permite al ESP32 comunicarse con otras redes y dispositivos.
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // Carga la configuración predeterminada de Wi-Fi(aca podemos revisar)
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // aca inicializamos el driver de Wi-Fi con la configuración predeterminada

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL); // Registra el event_handler para eventos de Wi-Fi
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    // 3. Intentar la conexión STA con las credenciales guardadas;
    // si no existen o falla la conexión, habilitar el AP de configuración.
    esp_ip4_addr_t server_ip;
    if (leer_credenciales_nvs() && strlen(rtu_ssid) > 0 && strlen(rtu_ssid) <= 32 &&
        strlen(rtu_pass) <= 63 &&
        esp_netif_str_to_ip4(rtu_host_ip, &server_ip) == ESP_OK) {
        ESP_LOGI(TAG, "Credenciales detectadas en memoria. Intentando conectar a SSID: %s", rtu_ssid);
        
        //Si hay credenciales guardadas, configuramos la Wi-Fi con esos datos
        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, rtu_ssid, strlen(rtu_ssid));
        memcpy(wifi_config.sta.password, rtu_pass, strlen(rtu_pass));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // Configura el ESP32 en modo estación (STA) para conectarse a un router Wi-Fi
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); // Configura la interfaz de red STA con las credenciales guardadas
        ESP_ERROR_CHECK(esp_wifi_start()); // Inicia el driver de Wi-Fi y comienza el proceso de conexión a la red especificada en wifi_config

        // Bloquea el hilo principal(main) hasta que la red conecte o falle rotundamente
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE, portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "¡Conectado! Lanzando tareas de telemetría...");
            // Wi-Fi ya se inicializó: se confirma el firmware OTA pendiente
            // de rollback y se inicia el cliente que envía telemetría y consulta FOTA.
            confirmar_app_si_valida();
            xTaskCreate(tls_client_task, "tls_client", 8192, NULL, 5, NULL);
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "Fallo crítico de conexión. Activando Fallback al Portal Cautivo.");
            iniciar_modo_ap();
            confirmar_app_si_valida();
        }
    } else {
        ESP_LOGW(TAG, "Configuración incompleta o inválida en NVS. Activando Portal Cautivo.");
        iniciar_modo_ap();
        confirmar_app_si_valida();
    }

    // 4. Mantener UART activa para permitir entrar al portal con el comando "config".
    printf("HOLA SOY GERMAN.\n");
    xTaskCreate(uart_monitor_task, "uart_monitor", 4096, NULL, 10, NULL);
}

// -----------------------------------------------------------------
// TAREA: CLIENTE TLS
// -----------------------------------------------------------------
// Envía todos los bytes solicitados, porque una escritura TLS puede ser parcial.
static bool tls_send_all(esp_tls_t *tls, const char *data, size_t length)
{
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

// Lee una línea de protocolo completa, terminada en '\n', sin exceder el buffer.
static bool tls_read_line(esp_tls_t *tls, char *line, size_t capacity)
{
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
    ESP_LOGE(TAG, "La respuesta del servidor excede el tamaño permitido.");
    return false;
}

// Convierte los 64 caracteres hexadecimales del SHA-256 en sus 32 bytes binarios.
static bool convertir_sha256(const char *hex, uint8_t digest[32])
{
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

// Recibe y valida un firmware ofrecido por el servidor en la conexión TLS actual.
static bool recibir_actualizacion_firmware(esp_tls_t *tls, const char *line)
{
    // La cabecera anuncia versión, tamaño y SHA-256 antes del bloque binario.
    char version[32] = {0};
    char image_size_text[11] = {0};
    char sha256_hex[65] = {0};
    int consumed = 0;
    if (sscanf(line, "OTA %31s %10s %64s%n",
               version, image_size_text, sha256_hex, &consumed) != 3 ||
        line[consumed] != '\n' || line[consumed + 1] != '\0' ||
        image_size_text[0] == '\0') {
        ESP_LOGE(TAG, "Cabecera OTA inválida.");
        return false;
    }

    for (size_t i = 0; image_size_text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)image_size_text[i])) {
            ESP_LOGE(TAG, "Tamaño de imagen OTA inválido.");
            return false;
        }
    }
    unsigned long long parsed_size = strtoull(image_size_text, NULL, 10);
    if (parsed_size == 0 || parsed_size > FOTA_MAX_IMAGE_SIZE) {
        ESP_LOGE(TAG, "Tamaño de imagen OTA fuera de rango.");
        return false;
    }
    uint32_t image_size = (uint32_t)parsed_size;

    uint8_t expected_digest[32];
    if (!convertir_sha256(sha256_hex, expected_digest)) {
        ESP_LOGE(TAG, "SHA-256 inválido en la cabecera OTA.");
        return false;
    }

    // ESP-IDF elige el slot OTA inactivo; nunca se escribe sobre la app en ejecución.
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || image_size > partition->size) {
        ESP_LOGE(TAG, "Imagen OTA (%u bytes) supera el tamaño del slot disponible.",
                 image_size);
        return false;
    }

    ESP_LOGI(TAG, "Recibiendo firmware %s (%u bytes) en %s.",
             version, image_size, partition->label);
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, image_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar OTA: %s", esp_err_to_name(err));
        return false;
    }

    // Inicializar el cálculo antes de descargar permite verificar el hash en streaming.
    psa_hash_operation_t sha_operation = PSA_HASH_OPERATION_INIT;
    bool success = psa_crypto_init() == PSA_SUCCESS &&
                   psa_hash_setup(&sha_operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    uint8_t buffer[1024];
    uint32_t received = 0;

    // Recibir en bloques mantiene bajo el uso de RAM aunque el firmware sea grande.
    while (success && received < image_size) {
        size_t remaining = image_size - received;
        size_t to_read = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int result = esp_tls_conn_read(tls, buffer, to_read);
        if (result <= 0) {
            ESP_LOGE(TAG, "Conexión TLS interrumpida durante la descarga: %d", result);
            success = false;
            break;
        }
        err = esp_ota_write(ota_handle, buffer, (size_t)result);
        if (err != ESP_OK ||
            psa_hash_update(&sha_operation, buffer, (size_t)result) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Error escribiendo o verificando la imagen OTA.");
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
        ESP_LOGE(TAG, "El SHA-256 recibido no coincide.");
        success = false;
    }
    if (!success) {
        psa_hash_abort(&sha_operation);
        esp_ota_abort(ota_handle);
        return false;
    }

    // esp_ota_end valida la estructura de la imagen y cierra la escritura OTA.
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "La imagen no es válida: %s", esp_err_to_name(err));
        return false;
    }

    esp_app_desc_t new_app;
    err = esp_ota_get_partition_description(partition, &new_app);
    if (err != ESP_OK || strncmp(version, new_app.version, sizeof(new_app.version)) != 0) {
        ESP_LOGE(TAG, "La versión declarada no coincide con la versión de la imagen.");
        return false;
    }

    // Solo después de validar contenido y versión se selecciona para el próximo arranque.
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo seleccionar la nueva imagen: %s", esp_err_to_name(err));
        return false;
    }

    char acknowledgment[48];
    int length = snprintf(acknowledgment, sizeof(acknowledgment), "OK %s\n", version);
    if (length <= 0 || (size_t)length >= sizeof(acknowledgment) ||
        !tls_send_all(tls, acknowledgment, (size_t)length)) {
        ESP_LOGW(TAG, "No se pudo confirmar la OTA al servidor; igualmente se reinicia.");
    }

    ESP_LOGI(TAG, "OTA instalada correctamente. Reiniciando para iniciar %s.", version);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

// Confirma una app marcada como pendiente una vez que la inicialización básica terminó.
// Si la app no llega hasta aquí, el bootloader puede restaurar el slot anterior.
static void confirmar_app_si_valida(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Validando el firmware OTA tras inicializar Wi-Fi.");
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    }
}

// Mantiene la conexión al servidor: envía telemetría, procesa NONE u OTA y reconecta.
static void tls_client_task(void *pvParameters)
{
    char payload[192];
    const esp_app_desc_t *app = esp_app_get_description(); // Lee la descripción de la app actual, que incluye versión y otros metadatos.
    for (;;)
    {
        // En modo de prueba se conserva el TLS sin validar el certificado del servidor.
        // Con FOTA_VERIFY_TLS_SERVER=1 se usa la CA incrustada y se valida el nombre.
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
            continue; // Si no se pudo inicializar TLS, esperar y reintentar
        }

        ESP_LOGI(TAG, "Iniciando Handshake TLS con %s:%d...", rtu_host_ip, rtu_tcp_port);
        int ret = esp_tls_conn_new_sync(rtu_host_ip, strlen(rtu_host_ip),
                                        rtu_tcp_port, &cfg, tls);

        if (ret == 1) {
            ESP_LOGI(TAG, "Conexión TLS establecida. Firmware actual: %s",
                     app->version);
            while (true) {
                // La versión que viaja en telemetría permite al servidor decidir si ofrecer OTA.
                int length = snprintf(payload, sizeof(payload),
                    "{\"sensor_id\":\"SENS-01\",\"fecha\":\"2026-10-06\","
                    "\"hora\":\"16:40:00\",\"firmware_version\":\"%s\"}\n",
                    app->version);
                if (length <= 0 || (size_t)length >= sizeof(payload) ||
                    !tls_send_all(tls, payload, (size_t)length)) { // tls_send_all envía todos los bytes de la telemetría al servidor
                    break;
                }

                char response[FOTA_LINE_SIZE]; // Lee la respuesta del servidor, que puede ser "NONE\n" o "OTA <version> <size> <sha256>\n"
                if (!tls_read_line(tls, response, sizeof(response))) {
                    break;
                }
                // Sin actualización, mantener la sesión y enviar la siguiente telemetría.
                if (strcmp(response, "NONE\n") == 0) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue; // Si la respuesta es "NONE", esperar 5 segundos
                }
                // Ante una oferta, recibir el binario y cerrar la sesión después del proceso.
                if (strncmp(response, "OTA ", 4) == 0) {
                    recibir_actualizacion_firmware(tls, response);
                    break;
                }
                ESP_LOGE(TAG, "Respuesta inesperada del servidor: %s", response);
                break;
            }
        }
        esp_tls_conn_destroy(tls);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// -----------------------------------------------------------------
// TAREA: ESCUCHAR UART (Siempre corriendo en Background)
// -----------------------------------------------------------------
// Permite activar el portal AP enviando "config" por UART y terminando con Enter.
static void uart_monitor_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0); //Instala el driver UART con un buffer de recepción de tamaño BUF_SIZE*2 y sin buffer de transmisión
    uart_param_config(UART_PORT_NUM, &uart_config); 
    uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); 

    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    int rx_index = 0;

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(UART_PORT_NUM, &byte, 1, portMAX_DELAY); //Lee un byte de datos del puerto UART y lo almacena en la variable 'byte'. La función espera indefinidamente hasta que se reciba un byte.

        if (len > 0) {
            if (byte == '\r' || byte == '\n') {
                if (rx_index > 0) {
                    data[rx_index] = '\0';
                    if (strcmp((char*)data, "config") == 0) {
                        ESP_LOGW("UART_MONITOR", "¡Comando recibido! Interrumpiendo todo para iniciar AP...");
                        // Si estábamos conectados, desconectamos a la fuerza
                        esp_wifi_disconnect();
                        iniciar_modo_ap();
                    }
                    rx_index = 0;
                }
            } else {
                if (rx_index < BUF_SIZE - 1) data[rx_index++] = byte;
            }
        }
    }
    free(data);
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------
// ESCANEO WI-FI Y SERVIDOR WEB (PASO 2)
// -----------------------------------------------------------------

// Devuelve el valor de un dígito hexadecimal o -1 si el carácter no es válido.
static int valor_hexadecimal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodifica campos de formulario URL (por ejemplo, '+' y secuencias '%xx').
// Devuelve false si la entrada está mal formada o no cabe en el destino.
static bool decodificar_url(char *destino, size_t capacidad, const char *origen) {
    size_t longitud = 0;
    while (*origen) {
        unsigned char caracter;
        if (*origen == '+') {
            caracter = ' ';
            origen++;
        } else if (*origen == '%') {
            int alto, bajo;
            if (!origen[1] || !origen[2] ||
                (alto = valor_hexadecimal(origen[1])) < 0 ||
                (bajo = valor_hexadecimal(origen[2])) < 0) {
                return false;
            }
            caracter = (unsigned char)((alto << 4) | bajo);
            origen += 3;
        } else {
            caracter = (unsigned char)*origen++;
        }

        if (caracter == '\0' || longitud + 1 >= capacidad) return false;
        destino[longitud++] = (char)caracter;
    }
    destino[longitud] = '\0';
    return true;
}

// Convierte un puerto decimal a entero y verifica que esté entre 1 y 65535.
static bool analizar_puerto(const char *texto, int *puerto) {
    unsigned int valor = 0;
    if (!*texto) return false;
    for (; *texto; texto++) {
        if (*texto < '0' || *texto > '9') return false;
        valor = valor * 10 + (unsigned int)(*texto - '0');
        if (valor > 65535) return false;
    }
    if (valor == 0) return false;
    *puerto = (int)valor;
    return true;
}

// Agrega texto al HTML si todavía hay espacio, manteniendo el terminador nulo.
static bool agregar_texto_html(char *html, size_t capacidad, size_t *longitud,
                               const char *texto, size_t texto_len) {
    if (texto_len >= capacidad - *longitud) return false;
    memcpy(html + *longitud, texto, texto_len);
    *longitud += texto_len;
    html[*longitud] = '\0';
    return true;
}

// Escapa caracteres especiales para insertar SSID u otros datos dentro del HTML.
static bool agregar_html_escapado(char *html, size_t capacidad, size_t *longitud,
                                  const char *texto, size_t texto_len) {
    for (size_t i = 0; i < texto_len; i++) {
        const char *entidad = NULL;
        switch (texto[i]) {
            case '&': entidad = "&amp;"; break;
            case '<': entidad = "&lt;"; break;
            case '>': entidad = "&gt;"; break;
            case '"': entidad = "&quot;"; break;
            case '\'': entidad = "&#39;"; break;
            default:
                if (!agregar_texto_html(html, capacidad, longitud, texto + i, 1)) return false;
                continue;
        }
        if (!agregar_texto_html(html, capacidad, longitud, entidad, strlen(entidad))) return false;
    }
    return true;
}


// Busca redes cercanas y guarda hasta MAX_REDES resultados para el formulario.
void escanear_redes_wifi(void) {
    ESP_LOGI("WIFI_SCAN", "Iniciando escaneo de redes Wi-Fi...");
    wifi_scan_config_t scan_config = { //Aca se configura el escaneo de redes Wi-Fi, indicando que no se filtren por SSID ni BSSID, y que se escaneen todos los canales
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true)); // true = Bloquea hasta terminar
    cantidad_redes = MAX_REDES;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&cantidad_redes, redes_encontradas));
    ESP_LOGI("WIFI_SCAN", "Escaneo finalizado. %d redes encontradas.", cantidad_redes);
}

// Atiende GET "/" y genera el formulario con las redes Wi-Fi detectadas.
static esp_err_t root_get_handler(httpd_req_t *req) {
    char *html = malloc(8192);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente.");
        return ESP_FAIL;
    }
    size_t longitud = 0;
    static const char inicio[] =
        "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>body{font-family:Arial;margin:20px;background:#f4f4f9;} .card{background:#fff;padding:20px;border-radius:8px;} "
        "input,select{width:100%;padding:10px;margin:5px 0 20px;border:1px solid #ccc;border-radius:4px;} "
        "button{width:100%;padding:12px;background:#0056b3;color:white;border:none;border-radius:4px;}</style>"
        "</head><body><div class=\"card\"><h2>RTU AgroConnect</h2><form action=\"/guardar\" method=\"POST\">"
        "<label>Red Wi-Fi (SSID):</label><br><select name=\"ssid\" required>";
    static const char campos_ip[] =
        "</select><label>Contraseña:</label><br><input type=\"password\" name=\"pass\">"
        "<label>IP del servidor TLS:</label><br><input type=\"text\" name=\"host_ip\" maxlength=\"15\" "
        "inputmode=\"decimal\" placeholder=\"IPv4 del servidor\" value=\"";
    static const char campos_puertos[] =
        "\"><label>Puerto TCP/TLS:</label><br><input type=\"number\" name=\"tcp_port\" value=\"";
    bool correcto =
        agregar_texto_html(html, 8192, &longitud, inicio, sizeof(inicio) - 1);

    for (int i = 0; correcto && i < cantidad_redes; i++) {
        size_t ssid_len = strnlen((char *)redes_encontradas[i].ssid,
                                  sizeof(redes_encontradas[i].ssid));
        if (ssid_len == 0) continue;
        static const char opcion_inicio[] = "<option value=\"";
        correcto = agregar_texto_html(html, 8192, &longitud,
                                      opcion_inicio, sizeof(opcion_inicio) - 1) &&
                   agregar_html_escapado(html, 8192, &longitud,
                                         (char *)redes_encontradas[i].ssid, ssid_len) &&
                   agregar_texto_html(html, 8192, &longitud, "\">", 2) &&
                   agregar_html_escapado(html, 8192, &longitud,
                                         (char *)redes_encontradas[i].ssid, ssid_len);
        if (correcto) {
            char rssi[24];
            int written = snprintf(rssi, sizeof(rssi), " (RSSI: %d)</option>", redes_encontradas[i].rssi);
            correcto = written > 0 && (size_t)written < sizeof(rssi) &&
                       agregar_texto_html(html, 8192, &longitud, rssi, (size_t)written);
        }
    }

    if (correcto) {
        correcto = agregar_texto_html(html, 8192, &longitud,
                                      campos_ip, sizeof(campos_ip) - 1) &&
            agregar_html_escapado(html, 8192, &longitud, rtu_host_ip, strlen(rtu_host_ip)) &&
            agregar_texto_html(html, 8192, &longitud,
                               campos_puertos, sizeof(campos_puertos) - 1);
    }
    if (correcto) {
        char ports[256];
        int written = snprintf(ports, sizeof(ports),
            "%d\" required><label>Puerto UDP:</label><br><input type=\"number\" name=\"udp_port\" value=\"%d\" required>"
            "<button type=\"submit\">Guardar y Reiniciar</button></form></div></body></html>",
            rtu_tcp_port, rtu_udp_port);
        correcto = written > 0 && (size_t)written < sizeof(ports) &&
                   agregar_texto_html(html, 8192, &longitud, ports, (size_t)written);
    }

    if (!correcto) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo generar la página.");
        return ESP_FAIL;
    }
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

// Atiende POST "/guardar": valida los campos, persiste la configuración y reinicia.
static esp_err_t guardar_post_handler(httpd_req_t *req) {
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "El formulario está vacío.");
        return ESP_FAIL;
    }
    if (req->content_len > MAX_FORM_BODY_SIZE) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "El formulario supera el tamaño permitido.");
        return ESP_OK;
    }

    size_t body_len = req->content_len;
    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente.");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < body_len) {
        int ret = httpd_req_recv(req, body + received, body_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No se pudo leer el formulario completo.");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[body_len] = '\0';

    // Primero se extraen los valores codificados del formulario y luego se decodifican.
    char ssid_raw[97] = {0}, pass_raw[190] = {0}, host_raw[46] = {0};
    char tcp_str[6] = {0}, udp_str[6] = {0};
    bool fields_ok =
        httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw)) == ESP_OK &&
        httpd_query_key_value(body, "pass", pass_raw, sizeof(pass_raw)) == ESP_OK &&
        httpd_query_key_value(body, "host_ip", host_raw, sizeof(host_raw)) == ESP_OK &&
        httpd_query_key_value(body, "tcp_port", tcp_str, sizeof(tcp_str)) == ESP_OK &&
        httpd_query_key_value(body, "udp_port", udp_str, sizeof(udp_str)) == ESP_OK;
    free(body);

    char ssid_dec[33] = {0}, pass_dec[65] = {0}, host_dec[16] = {0};
    int tcp_port, udp_port;
    esp_ip4_addr_t parsed_host;
    fields_ok = fields_ok &&
        decodificar_url(ssid_dec, sizeof(ssid_dec), ssid_raw) &&
        decodificar_url(pass_dec, sizeof(pass_dec), pass_raw) &&
        decodificar_url(host_dec, sizeof(host_dec), host_raw) &&
        strlen(ssid_dec) > 0 && strlen(ssid_dec) <= 32 &&
        strlen(pass_dec) <= 63 &&
        esp_netif_str_to_ip4(host_dec, &parsed_host) == ESP_OK &&
        analizar_puerto(tcp_str, &tcp_port) &&
        analizar_puerto(udp_str, &udp_port);
    if (!fields_ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Datos inválidos: revise SSID, contraseña, IP y puertos.");
        return ESP_FAIL;
    }

    // Solo guardar después de validar SSID, contraseña, dirección y puertos.
    guardar_credenciales_nvs(ssid_dec, pass_dec, host_dec, tcp_port, udp_port);

    // 2. Avisar al usuario por el navegador
    const char *respuesta = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>"
                            "<body style=\"font-family:Arial;text-align:center;margin-top:50px;\">"
                            "<h2>¡Configuracion Guardada!</h2><p>La RTU se esta reiniciando para conectar a la nueva red...</p></body></html>";
    httpd_resp_send(req, respuesta, HTTPD_RESP_USE_STRLEN);

    // 3. Forzar el reinicio por hardware (Damos 1 segundo para que el navegador reciba la respuesta)
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();

    return ESP_OK;
}

// Mapeo entre las rutas HTTP del portal y las funciones que las atienden.
static const httpd_uri_t uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL
};
static const httpd_uri_t uri_guardar = {
    .uri = "/guardar", .method = HTTP_POST, .handler = guardar_post_handler, .user_ctx = NULL
};

// Inicia el servidor HTTP local y registra las rutas de página y guardado.
static httpd_handle_t iniciar_servidor_web(void) {
    httpd_handle_t server = NULL; // 
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); 
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_guardar); 
        ESP_LOGI("HTTP_SERVER", "Servidor web HTTP lanzado.");
        return server;
    }
    return NULL;
}

// -----------------------------------------------------------------
// ACTIVACIÓN DEL ACCESS POINT
// -----------------------------------------------------------------
// Levanta la red de configuración RTU_AGC y publica el portal en 192.168.4.1.
void iniciar_modo_ap(void) {
    ESP_LOGI("WIFI_AP", "Configurando red propia...");

    modo_configuracion = true;
    esp_wifi_disconnect();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "RTU_AGC",
            .ssid_len = strlen("RTU_AGC"),
            .password = "agroconnect",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    esp_wifi_start();

    // NUEVO: Escaneamos el espectro RF antes de arrancar el servidor
    escanear_redes_wifi();

    ESP_LOGW("WIFI_AP", "=================================================");
    ESP_LOGW("WIFI_AP", "      MODO DE CONFIGURACIÓN LOCAL ACTIVADO       ");
    ESP_LOGW("WIFI_AP", "1. Conéctate a la red Wi-Fi: RTU_AGC");
    ESP_LOGW("WIFI_AP", "2. Contraseña: agroconnect");
    ESP_LOGW("WIFI_AP", "3. Abre tu navegador web e ingresa a: http://192.168.4.1");
    ESP_LOGW("WIFI_AP", "=================================================");

    iniciar_servidor_web();
}
