#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "web_portal.h"

static const char *TAG = "WIFI_MGR";

/* Bits de sincronización para la conexión Wi-Fi */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;
static bool s_modo_configuracion = false;

/* Buffer interno para el escaneo de redes */
static wifi_ap_record_t s_redes_encontradas[WIFI_MAX_SCAN_REDES];
static uint16_t s_cantidad_redes = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_modo_configuracion) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_modo_configuracion && s_retry_num < WIFI_MAX_REINTENTOS) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Reintentando conexión al router (%d/%d)...",
                     s_retry_num, WIFI_MAX_REINTENTOS);
        } else if (!s_modo_configuracion) {
            ESP_LOGW(TAG, "Se superó el límite de reintentos.");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void){
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "No se pudo crear el EventGroup de Wi-Fi.");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Controlador Wi-Fi inicializado.");
    return ESP_OK;
}

bool wifi_manager_conectar_sta(const app_config_t *config){
    if (!config) {
        return false;
    }

    s_modo_configuracion = false;
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, config->pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "Error iniciando Wi-Fi STA: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Conectando a SSID: '%s'...", config->ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_escanear_redes(void){
    ESP_LOGI(TAG, "Iniciando escaneo de redes Wi-Fi...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        s_cantidad_redes = WIFI_MAX_SCAN_REDES;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&s_cantidad_redes, s_redes_encontradas));
        ESP_LOGI(TAG, "Escaneo finalizado. %d redes encontradas.", s_cantidad_redes);
    } else {
        ESP_LOGE(TAG, "Fallo al escanear redes: %s", esp_err_to_name(err));
    }
}

uint16_t wifi_manager_get_redes_escaneadas(const wifi_ap_record_t **redes){
    if (redes) {
        *redes = s_redes_encontradas;
    }
    return s_cantidad_redes;
}

void wifi_manager_iniciar_ap(void){
    ESP_LOGI(TAG, "Configurando Access Point local de rescate...");

    s_modo_configuracion = true;
    esp_wifi_disconnect();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGE(TAG, "Error iniciando Wi-Fi AP: %s", esp_err_to_name(err));
    }

    // Escanear redes disponibles para mostrarlas en la lista desplegable web
    wifi_manager_escanear_redes();

    ESP_LOGW(TAG, "=================================================");
    ESP_LOGW(TAG, "      MODO DE CONFIGURACIÓN LOCAL ACTIVADO       ");
    ESP_LOGW(TAG, "1. Conéctate a la red Wi-Fi: %s", WIFI_AP_SSID);
    ESP_LOGW(TAG, "2. Contraseña: %s", WIFI_AP_PASS);
    ESP_LOGW(TAG, "3. Abre tu navegador web en: http://192.168.4.1");
    ESP_LOGW(TAG, "=================================================");

    // Iniciar el servidor web local
    web_portal_iniciar();
}

bool wifi_manager_en_modo_configuracion(void){
    return s_modo_configuracion;
}

void wifi_manager_desconectar(void){
    esp_wifi_disconnect();
}
