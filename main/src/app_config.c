#include "app_config.h"

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"

static const char *TAG = "APP_CONFIG";

#define NVS_STORAGE_NAMESPACE "storage"

esp_err_t app_config_init_nvs(void){
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Partición NVS sin páginas libres o nueva versión detectada. Borrando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "NVS inicializado correctamente.");
    }

    return ret;
}

bool app_config_cargar(app_config_t *config){
    if (!config) {
        return false;
    }

    // Limpiamos la estructura y asignamos puertos por defecto preventivamente
    memset(config, 0, sizeof(app_config_t));
    config->tcp_port = CONFIG_DEFAULT_TCP_PORT;
    config->udp_port = CONFIG_DEFAULT_UDP_PORT;

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se encontró espacio NVS '%s' (posible primer inicio).", NVS_STORAGE_NAMESPACE);
        return false;
    }

    // Leemos el SSID (campo obligatorio)
    size_t size = sizeof(config->ssid);
    err = nvs_get_str(my_handle, "ssid", config->ssid, &size);
    if (err != ESP_OK || strlen(config->ssid) == 0) {
        nvs_close(my_handle);
        return false;
    }

    // Contraseña
    size = sizeof(config->pass);
    nvs_get_str(my_handle, "pass", config->pass, &size);

    // IP del servidor
    size = sizeof(config->host_ip);
    nvs_get_str(my_handle, "host_ip", config->host_ip, &size);

    // Puertos TCP y UDP
    nvs_get_i32(my_handle, "tcp_port", (int32_t *)&config->tcp_port);
    nvs_get_i32(my_handle, "udp_port", (int32_t *)&config->udp_port);

    nvs_close(my_handle);

    ESP_LOGI(TAG, "Configuración leída de NVS: SSID='%s', Host='%s', TCP=%d, UDP=%d",
             config->ssid, config->host_ip, config->tcp_port, config->udp_port);

    return true;
}

esp_err_t app_config_guardar(const app_config_t *config){
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al abrir NVS para escritura: %s", esp_err_to_name(err));
        return err;
    }

    // Escritura encadenada de parámetros
    err = nvs_set_str(my_handle, "ssid", config->ssid);
    if (err == ESP_OK) err = nvs_set_str(my_handle, "pass", config->pass);
    if (err == ESP_OK) err = nvs_set_str(my_handle, "host_ip", config->host_ip);
    if (err == ESP_OK) err = nvs_set_i32(my_handle, "tcp_port", (int32_t)config->tcp_port);
    if (err == ESP_OK) err = nvs_set_i32(my_handle, "udp_port", (int32_t)config->udp_port);

    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Credenciales y puertos guardados con éxito en NVS.");
        } else {
            ESP_LOGE(TAG, "Error en nvs_commit: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Error escribiendo parámetros en NVS: %s", esp_err_to_name(err));
    }

    nvs_close(my_handle);
    return err;
}

bool app_config_validar(const app_config_t *config){
    if (!config) {
        return false;
    }

    // Longitud de SSID: entre 1 y 32 caracteres
    size_t ssid_len = strlen(config->ssid);
    if (ssid_len == 0 || ssid_len >= CONFIG_SSID_MAX_LEN) {
        return false;
    }

    // Longitud de clave WPA2: máximo 64 caracteres
    if (strlen(config->pass) >= CONFIG_PASS_MAX_LEN) {
        return false;
    }

    // Validación formal de IPv4
    esp_ip4_addr_t ip_check;
    if (esp_netif_str_to_ip4(config->host_ip, &ip_check) != ESP_OK) {
        return false;
    }

    // Rango válido de puertos TCP/UDP (1 - 65535)
    if (config->tcp_port <= 0 || config->tcp_port > 65535) {
        return false;
    }

    if (config->udp_port <= 0 || config->udp_port > 65535) {
        return false;
    }

    return true;
}
