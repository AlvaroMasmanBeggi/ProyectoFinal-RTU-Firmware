| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | ESP32-S31 | Linux |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | --------- | ----- |

# Hello World Example

Starts a FreeRTOS task to print "Hello World".

## FOTA por TLS

La RTU mantiene una conexión TLS saliente al servidor configurado. Cada cinco
segundos envía una línea JSON de telemetría con `firmware_version`; el servidor
responde `NONE\n` o envía `OTA <version> <bytes> <sha256>\n` seguido de esos
bytes exactos del archivo `.bin`. Esto funciona con dispositivos detrás de NAT:
el servidor no abre una conexión entrante a la ESP32.

El cliente escribe la imagen en la partición OTA inactiva, comprueba SHA-256 y
la versión guardada dentro del binario, y usa la validación de imagen de ESP-IDF
antes de seleccionarla para el próximo arranque. El rollback de bootloader queda
habilitado; la aplicación marca la nueva imagen válida cuando la conexión Wi-Fi
o el portal AP ya están inicializados.

### Migración inicial y compilación

La tabla de particiones ahora usa dos slots OTA de 960 KiB para flash de 2 MB.
Hay que volver a grabar una vez por USB (bootloader, tabla de particiones y
aplicación); una instalación que conserva la tabla anterior de una sola
aplicación no puede recibir esta primera actualización OTA. El binario debe
caber en un slot; la aplicación usa optimización por tamaño para dejar margen.
Si el dispositivo tiene más flash, se puede seleccionar una tabla OTA de mayor
capacidad en `menuconfig`.

Incrementá la versión del proyecto en `project(... VERSION ...)` en
`CMakeLists.txt` para cada firmware publicado. El valor debe coincidir con
`esp_app_get_description()->version` que la RTU envía al servidor.

### Publicar firmware

Con certificados y llave de servidor ya configurados como en la prueba actual:

```powershell
idf.py -B build_fota build
python servidor_tls_json.py --firmware build_fota\hello_world.bin --version 1.0.1
```

`--version` debe ser exactamente la versión embebida en ese binario. El servidor
solo envía el firmware cuando recibe una RTU con una versión diferente. La RTU
confirma `OK <version>` después de instalarlo; tras reiniciar, el servidor deja
de ofrecer esa misma versión.

Para mantener solo la recepción de telemetría, iniciar el servidor sin
`--firmware` ni `--version`. El puerto TCP predeterminado es 3334.

### Verificación TLS

Para conservar el modo de prueba existente, `FOTA_VERIFY_TLS_SERVER` está en
`0` al comienzo de `main/hello_world_main.c`: en ese modo TLS cifra el canal,
pero no autentica al servidor. **No usarlo en producción**, porque un atacante
capaz de interceptar la conexión podría distribuir firmware propio.

Para habilitar verificación, reemplazá `main/ota_server_ca.pem` por la CA/cadena
que firma el certificado del servidor, cambiá `FOTA_VERIFY_TLS_SERVER` a `1` y
recompilá. El host configurado en la RTU debe coincidir con el SAN del
certificado (si se conecta por IP, el certificado debe incluir esa IP). En
producción, usar además Secure Boot y una política de anti-rollback.

(See the README.md file in the upper level 'examples' directory for more information about examples.)

## How to use example

Follow detailed instructions provided specifically for this example.

Select the instructions depending on Espressif chip installed on your development board:

- [ESP32 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
- [ESP32-S2 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/get-started/index.html)


## Example folder contents

The project **hello_world** contains one source file in C language [hello_world_main.c](main/hello_world_main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt` files that provide set of directives and instructions describing the project's source files and targets (executable, library, or both).

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── pytest_hello_world.py      Python script used for automated testing
├── main
│   ├── CMakeLists.txt
│   └── hello_world_main.c
└── README.md                  This is the file you are currently reading
```

For more information on structure and contents of ESP-IDF projects, please refer to Section [Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html) of the ESP-IDF Programming Guide.

## Troubleshooting

* Program upload failure

    * Hardware connection is not correct: run `idf.py -p PORT monitor`, and reboot your board to see if there are any output logs.
    * The baud rate for downloading is too high: lower your baud rate in the `menuconfig` menu, and try again.

## Technical support and feedback

Please use the following feedback channels:

* For technical queries, go to the [esp32.com](https://esp32.com/) forum
* For a feature request or bug report, create a [GitHub issue](https://github.com/espressif/esp-idf/issues)

We will get back to you as soon as possible.
