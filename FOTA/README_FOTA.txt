Se implementó FOTA por la conexión TLS saliente que ya mantiene la ESP32. El dispositivo informa su versión junto con la telemetría;
el servidor responde NONE o envía una cabecera con versión, tamaño y SHA‑256, seguida del firmware. 
La ESP32 verifica la transferencia y la imagen antes de seleccionarla para el próximo arranque.

---------------------------------------+INFO PARA ENTENDER ESTO--------------------------------------------

SHA-256 es una función que calcula una huella digital de los datos: produce siempre un valor de 256 bits (32 bytes) para un archivo. 
El servidor calcula la huella del firmware y la envía a la ESP32; la ESP32 calcula la suya mientras lo recibe y las compara. Si no coinciden, 
la descarga se dañó o cambió y no se instala.

La huella detecta cambios, pero por sí sola no demuestra quién envió el archivo: alguien que pudiera sustituir el firmware también podría 
reemplazar la huella. Para eso es importante autenticar al servidor con TLS verificado y, como protección adicional, usar firma de firmware/Secure Boot.

Rollback es volver automáticamente al firmware anterior si el nuevo no arranca o no logra validarse. Para hacerlo, se necesitan dos particiones 
de aplicación: la ESP32 conserva la versión actual en una y escribe la nueva en la otra. Si el arranque nuevo falla, el bootloader puede 
regresar a la anterior; si funciona, la aplicación confirma que la actualización es válida.

-----------------------------------------------------------------------------------------------------------

Con esta sentencia compilo el nuevo firmware desde ESP IDF:   

	idf.py -B build_fota build

Con esta sentencia desde el shell abro el servidor pero le paso el firmware a enviar: 

	python servidor_tls_json.py --firmware build_fota\hello_world.bin --version 1.0.1

La versión debe coincidir con la embebida en el binario; hay que incrementarla en project(... VERSION ...) para cada publicación.Esto esta en el CMake.
El servidor entrega el firmware cuando la ESP32 se conecta y reporta una versión distinta.

1. QUE HACE LA IMPLEMENTACIÓN FOTA?

La RTU no espera una conexión entrante del servidor. En cambio, se conecta por Wi‑Fi al servidor de la misma manera que ya lo hacía para enviar
telemetría. Eso permite que funcione detrás de un router o NAT: el dispositivo inicia la conexión y el servidor responde por ese mismo canal TLS.

En cada conexión, el intercambio es:

A - La ESP32 establece TLS con el servidor configurado.
B - Envía una línea JSON de telemetría, ahora con la versión del firmware:

	{"sensor_id":"SENS-01","fecha":"2026-10-06","hora":"16:40:00","firmware_version":"1.0.1"}

El servidor responde con una de estas opciones:

A - NONE\n: no hay una actualización para enviar.
B - OTA <version> <bytes> <sha256>\n, seguido inmediatamente por esa cantidad exacta de bytes del archivo .bin.

Si recibe firmware, la ESP32 lo escribe en la partición OTA inactiva y comprueba la imagen.
Si todo es válido, selecciona esa partición para el próximo arranque, responde OK <version>\n y se reinicia.
El envío de telemetría y la negociación están en tls_client_task; el servidor implementa el protocolo en handle_client.

2. QUE SE CAMBIÓ EN EL ESP32, CON RESPECTO AL CODIGO QUE SOLO ENVIABA TELEMETRÍA(CARPETA CONFIG_AP)

En main.c se añadió estas piezas:

* Envío completo por TLS: tls_send_all repite la escritura si TLS acepta solo parte de los datos.

* Lectura de líneas de protocolo: tls_read_line recibe la respuesta del servidor hasta el salto de línea y limita su tamaño.

* Validación de la cabecera: antes de comenzar, comprueba que la versión, el tamaño y el SHA‑256 tengan el formato esperado. Limita el firmware a 
1 MiB y comprueba que entre en el slot disponible.

* Descarga por bloques: lee hasta 1024 bytes por iteración, los escribe con esp_ota_write y actualiza el SHA‑256 al mismo tiempo. Así no hace falta
cargar el firmware entero en la RAM de la ESP32.

* Comprobaciones antes de reiniciar: compara el SHA‑256 recibido, valida la imagen con ESP-IDF y comprueba que la versión declarada coincida con 
la versión dentro del binario. Si algo falla, no selecciona la imagen para arrancar.

* Rollback: si la aplicación recién instalada no llega a inicializarse adecuadamente en el siguiente arranque, el bootloader puede volver a 
la aplicación anterior. El código marca la nueva aplicación como válida cuando ya se conectó por Wi‑Fi o activó el AP.

La lógica FOTA se ejecuta después de iniciar Wi‑Fi y TLS. El portal AP, el formulario de configuración y el comando UART siguen cumpliendo 
sus funciones existentes.

3. QUE SE CAMBIO EN EL SERVIDOR
En servidor_tls_json.py el servidor ahora:

* Sigue recibiendo e imprimiendo telemetría.

* Puede iniciarse sin firmware y responder NONE, como servidor de telemetría.

* Puede cargar una imagen .bin con --firmware, calcular su SHA‑256 y anunciarla con --version.

* Espera una confirmación OK <version> después de enviarla.

* Usa TLS 1.2 como mínimo y conserva el uso de server.crt y server.key.

* La imagen completa se carga en la memoria del servidor, no en la de la ESP32.

Importante: el servidor actual considera que hay que actualizar cuando la versión reportada es distinta de la publicada; no compara cuál versión 
es más nueva. Por eso, publicá siempre una versión mayor que la instalada. Esta prueba tampoco incluye una política de anti-downgrade.

4. PARTICIONES, VERSION Y TLS

La tabla anterior de memoria era de una sola aplicación. En ese diseño no hay una segunda partición donde escribir el firmware nuevo sin tocar 
el que está ejecutándose. Por eso se agregó partitions.csv con:

* NVS para la configuración existente.

* otadata, que permite al bootloader recordar cuál aplicación debe iniciar.

* Dos particiones de aplicación OTA de 960 KiB cada una.

* También se habilitó el rollback y la optimización por tamaño en sdkconfig. Ojo con los tamaños de los .bin y el espacio free en las particiones.

* La versión del proyecto está en CMakeLists.txt. Esa versión queda embebida en el firmware; la ESP32 se la informa al servidor y la compara 
con la cabecera OTA. Hay que cambiarla para cada versión que se publique.

Para las pruebas, FOTA_VERIFY_TLS_SERVER está en 0, es decir, no verifica la autenticidad del servidor. TLS cifra la comunicación, pero en esta 
configuración la ESP32 no verifica que el servidor sea realmente el esperado. El certificado actual queda embebido como plantilla 
en ota_server_ca.pem, listo para sustituirlo y habilitar la comprobación más adelante. Para que funcione ota_server_ca.pem es exactamente el mismo
que server.crt que es el certificado del servidor. Hay que implementar también Secure Boot.

PARA SUBIR UN NUEVO FIRMWARE A DISTANCIA:
1- En el CmakeList.txt cambiamos la versión a una mas nueva, si dice 1.0.1 pongo 1.0.2
2- Compilo idf.py -B build_fota build
3- Si quiero ver informacion sobre la compilacion para por ejemplo chequear la version uso: 
	python -m esptool --chip esp32 image-info build_fota\hello_world.bin
4- Arranco el server con un firmaware para enviar con: python servidor_tls_json.py --firmware build_fota\hello_world.bin --version 1.0.2