#include "web_portal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "app_config.h"
#include "wifi_manager.h"

static const char *TAG = "WEB_PORTAL";
static httpd_handle_t s_server = NULL;

#define MAX_FORM_BODY_SIZE 512
#define HTML_BUFFER_SIZE   8192

/* --- Funciones auxiliares de parseo y codificación URL --- */
static int valor_hexadecimal(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decodificar_url(char *destino, size_t capacidad, const char *origen){
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

        if (caracter == '\0' || longitud + 1 >= capacidad) {
            return false;
        }
        destino[longitud++] = (char)caracter;
    }
    destino[longitud] = '\0';
    return true;
}

static bool analizar_puerto(const char *texto, int *puerto){
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

static bool agregar_texto_html(char *html, size_t capacidad, size_t *longitud, const char *texto, size_t texto_len){
    if (texto_len >= capacidad - *longitud) return false;
    memcpy(html + *longitud, texto, texto_len);
    *longitud += texto_len;
    html[*longitud] = '\0';
    return true;
}

static bool agregar_html_escapado(char *html, size_t capacidad, size_t *longitud, const char *texto, size_t texto_len){
    for (size_t i = 0; i < texto_len; i++) {
        const char *entidad = NULL;
        switch (texto[i]) {
            case '&':  entidad = "&amp;"; break;
            case '<':  entidad = "&lt;"; break;
            case '>':  entidad = "&gt;"; break;
            case '"':  entidad = "&quot;"; break;
            case '\'': entidad = "&#39;"; break;
            default:
                if (!agregar_texto_html(html, capacidad, longitud, texto + i, 1)) return false;
                continue;
        }
        if (!agregar_texto_html(html, capacidad, longitud, entidad, strlen(entidad))) return false;
    }
    return true;
}

/* --- Handlers HTTP --- */

static esp_err_t root_get_handler(httpd_req_t *req){
    char *html = malloc(HTML_BUFFER_SIZE);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente.");
        return ESP_FAIL;
    }

    // Leemos la configuración actual para precargar IP y puertos
    app_config_t config;
    app_config_cargar(&config);

    // Obtenemos la lista de redes detectadas por wifi_manager
    const wifi_ap_record_t *redes = NULL;
    uint16_t cantidad_redes = wifi_manager_get_redes_escaneadas(&redes);

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

    bool correcto = agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, inicio, sizeof(inicio) - 1);

    // Generamos las opciones del <select> con las redes escaneadas
    for (int i = 0; correcto && i < cantidad_redes; i++) {
        size_t ssid_len = strnlen((char *)redes[i].ssid, sizeof(redes[i].ssid));
        if (ssid_len == 0) continue;

        static const char opcion_inicio[] = "<option value=\"";
        correcto = agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, opcion_inicio, sizeof(opcion_inicio) - 1) &&
                   agregar_html_escapado(html, HTML_BUFFER_SIZE, &longitud, (char *)redes[i].ssid, ssid_len) &&
                   agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, "\">", 2) &&
                   agregar_html_escapado(html, HTML_BUFFER_SIZE, &longitud, (char *)redes[i].ssid, ssid_len);

        if (correcto) {
            char rssi[32];
            int written = snprintf(rssi, sizeof(rssi), " (RSSI: %d dBm)</option>", redes[i].rssi);
            correcto = written > 0 && (size_t)written < sizeof(rssi) &&
                       agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, rssi, (size_t)written);
        }
    }

    if (correcto) {
        correcto = agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, campos_ip, sizeof(campos_ip) - 1) &&
                   agregar_html_escapado(html, HTML_BUFFER_SIZE, &longitud, config.host_ip, strlen(config.host_ip)) &&
                   agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, campos_puertos, sizeof(campos_puertos) - 1);
    }

    if (correcto) {
        char ports[256];
        int written = snprintf(ports, sizeof(ports),
            "%d\" required><label>Puerto UDP:</label><br><input type=\"number\" name=\"udp_port\" value=\"%d\" required>"
            "<button type=\"submit\">Guardar y Reiniciar</button></form></div></body></html>",
            config.tcp_port, config.udp_port);
        correcto = written > 0 && (size_t)written < sizeof(ports) &&
                   agregar_texto_html(html, HTML_BUFFER_SIZE, &longitud, ports, (size_t)written);
    }

    if (!correcto) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo generar la página web.");
        return ESP_FAIL;
    }

    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t guardar_post_handler(httpd_req_t *req){
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

    // Extracción de valores sin decodificar
    char ssid_raw[97] = {0}, pass_raw[190] = {0}, host_raw[46] = {0};
    char tcp_str[6] = {0}, udp_str[6] = {0};

    bool fields_ok =
        httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw)) == ESP_OK &&
        httpd_query_key_value(body, "pass", pass_raw, sizeof(pass_raw)) == ESP_OK &&
        httpd_query_key_value(body, "host_ip", host_raw, sizeof(host_raw)) == ESP_OK &&
        httpd_query_key_value(body, "tcp_port", tcp_str, sizeof(tcp_str)) == ESP_OK &&
        httpd_query_key_value(body, "udp_port", udp_str, sizeof(udp_str)) == ESP_OK;
    free(body);

    app_config_t nueva_config = {0};
    int tcp_port = 0, udp_port = 0;

    fields_ok = fields_ok &&
        decodificar_url(nueva_config.ssid, sizeof(nueva_config.ssid), ssid_raw) &&
        decodificar_url(nueva_config.pass, sizeof(nueva_config.pass), pass_raw) &&
        decodificar_url(nueva_config.host_ip, sizeof(nueva_config.host_ip), host_raw) &&
        analizar_puerto(tcp_str, &tcp_port) &&
        analizar_puerto(udp_str, &udp_port);

    nueva_config.tcp_port = tcp_port;
    nueva_config.udp_port = udp_port;

    // Validación formal centralizada mediante app_config
    if (!fields_ok || !app_config_validar(&nueva_config)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Datos inválidos: revise SSID, contraseña, IP y puertos.");
        return ESP_FAIL;
    }

    // Persistir configuración en NVS
    app_config_guardar(&nueva_config);

    // Respuesta informativa al cliente web
    const char *respuesta =
        "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "</head><body style=\"font-family:Arial;text-align:center;margin-top:50px;\">"
        "<h2>¡Configuración Guardada!</h2><p>La RTU se está reiniciando para conectar a la nueva red...</p>"
        "</body></html>";
    httpd_resp_send(req, respuesta, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Configuración guardada. Reiniciando equipo en 1 segundo...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static const httpd_uri_t uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL
};

static const httpd_uri_t uri_guardar = {
    .uri = "/guardar", .method = HTTP_POST, .handler = guardar_post_handler, .user_ctx = NULL
};

esp_err_t web_portal_iniciar(void){
    if (s_server != NULL) {
        ESP_LOGW(TAG, "El servidor web ya está en ejecución.");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret == ESP_OK) {
        httpd_register_uri_handler(s_server, &uri_root);
        httpd_register_uri_handler(s_server, &uri_guardar);
        ESP_LOGI(TAG, "Servidor web HTTP lanzado exitosamente.");
    } else {
        ESP_LOGE(TAG, "Error al iniciar servidor HTTP: %s", esp_err_to_name(ret));
    }
    return ret;
}

void web_portal_detener(void){
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Servidor web HTTP detenido.");
    }
}
