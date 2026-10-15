# RTU AgroConnect - Firmware Base (ESP-IDF)

Diseño de RTU como proyecto final de Ingenieria Electrónica - UTN - FRM

Firmware modular para Unidad Terminal Remota (RTU) basada en ESP32-S3, diseñada para aplicaciones agrícolas e industriales de telemetría, control y adquisición de datos en campo.

El sistema incorpora conectividad segura TLS, gestión dinámica de red mediante portal cautivo SoftAP, comandos de control serie y actualizaciones remotas de firmware sobre la marcha (**FOTA**) con verificación criptográfica y protección contra fallos (*anti-rollback*).

---

## 🚀 Características Principales

* **Arquitectura Modular en C:** Código desacoplado en módulos independientes (`app_config`, `wifi_manager`, `web_portal`, `fota_service`, `uart_console`).
* **Conexión Wi-Fi Resiliente:** Reconexión automática con límite de reintentos y activación de modo de contingencia (*fallback*).
* **Portal Cautivo de Configuración:** Servidor web HTTP embebido (`192.168.4.1`) que permite escanear redes Wi-Fi locales, configurar credenciales y definir parámetros de red (IP de host y puertos TCP/UDP) persistidos en memoria Flash (NVS).
* **Consola de Comandos UART:** Tarea en segundo plano sobre UART0 (115200 bps) para invocar el modo de configuración mediante el comando `"config"`.
* **Telemetría y FOTA Seguro (TLS):** 
  * Conexión saliente cliente TLS hacia el servidor central (apto para redes detrás de NAT o firewalls).
  * Envío periódico de telemetría en formato JSON con la versión activa de la aplicación.
  * Descarga y flasheo de firmware en streaming (bloques de 1 KiB) con cálculo y comprobación de hash **SHA-256** al vuelo mediante aceleración criptográfica PSA Crypto.
  * Esquema **Dual-OTA con Rollback automático**: si un firmware nuevo no inicializa correctamente, el bootloader revierte de forma autónoma al slot previo funcional.

---

## 📁 Estructura del Proyecto

```text
├── CMakeLists.txt              <- Configuración general del proyecto ESP-IDF
├── partitions.csv              <- Tabla de particiones personalizada (Dual-OTA + NVS)
├── certs/
│   └── ota_server_ca.pem       <- Certificado CA del servidor (verificación TLS)
├── main/
│   ├── CMakeLists.txt          <- Registro de fuentes, includes y dependencias
│   ├── app_main.c              <- Orquestador y punto de entrada de la aplicación
│   ├── include/
│   │   ├── app_config.h        <- Definiciones y persistencia en memoria NVS
│   │   ├── wifi_manager.h      <- Gestión de interfaz STA, SoftAP y escaneo RF
│   │   ├── web_portal.h        <- Servidor HTTP local y procesamiento de formularios
│   │   ├── fota_service.h      <- Cliente TLS, telemetría y motor de actualización
│   │   └── uart_console.h      <- Driver y monitor serie de comandos
│   └── src/
│       ├── app_config.c
│       ├── wifi_manager.c
│       ├── web_portal.c
│       ├── fota_service.c
│       └── uart_console.c
├── README.md                   
└── README_FOTA.md              <- Manual técnico detallado del subsistema FOTA
