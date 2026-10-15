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

Para iniciar el servidor sin firmware y responder NONE mientras se prepara una actualización:

	python .\py_files\servidor_tls_json.py

Para verificar la version del .bin ejecutar:

 	python -m esptool --chip esp32 image-info .\build\ProyectoFinal-RTU.bin

Si ya estás dentro de la carpeta py_files, puedes ejecutar `python .\servidor_tls_json.py`.
El servidor abre la interfaz de control local en http://127.0.0.1:8080. Desde allí se selecciona
el archivo .bin y se escribe su versión. No es necesario detener ni volver a iniciar el servidor:
la imagen se carga en memoria y se ofrece en el siguiente mensaje de telemetría de la RTU.
Si la versión reportada por la RTU coincide con la publicada, el servidor responde NONE.
La versión introducida debe coincidir con la embebida en el binario.

También se puede iniciar el servidor publicando una imagen desde la línea de comandos:

	python .\py_files\servidor_tls_json.py --firmware .\build_fota\hello_world.bin --version 1.0.1

La interfaz se liga únicamente a 127.0.0.1, así que solo se puede administrar desde la misma PC.

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

* Incluye una interfaz web local en http://127.0.0.1:8080 para publicar o retirar firmware sin reiniciar el servidor.

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

* La versión incluida en el firmware se puede consultar con esptool (`image-info`). La RTU la informa al servidor y la compara con la versión
anunciada en la cabecera OTA; ambas deben coincidir con la versión real del binario.

La ESP32 valida el certificado TLS del servidor usando `certs/ota_server_ca.pem`, que debe coincidir con el certificado configurado en
`py_files/server.crt`. El certificado de prueba está emitido para `localhost`, mientras que la RTU se conecta a una dirección IP; por eso se
omite la comparación del nombre del host y se fija como confiable ese certificado concreto. Para producción, instala un certificado emitido para
el nombre DNS/IP del servidor y habilita su verificación de nombre. SHA-256 valida la integridad de la imagen, pero no sustituye una firma de
firmware ni Secure Boot.

PARA PROBAR UNA ACTUALIZACIÓN SIN REINICIAR EL SERVIDOR:
1- Compilo el firmware con la versión que quiero probar.
2- Verifico la versión incluida en el binario con:
	python -m esptool --chip esp32 image-info build_fota\hello_world.bin
3- Inicio una sola vez el servidor con python .\py_files\servidor_tls_json.py.
4- Abro http://127.0.0.1:8080, selecciono el .bin, escribo la versión que muestra image-info y publico.
5- La RTU actualiza si su firmware_version es distinta; si es igual, el servidor responde NONE.
6- Para volver al modo sin actualización, pulso “Retirar firmware y responder NONE”.

El servidor atiende cada conexión TLS en su propio hilo y limita a 10 segundos el handshake. Si ya había una instancia
anterior ejecutándose antes de esta mejora, hay que detenerla con Ctrl+C e iniciarla de nuevo para cargar el cambio.