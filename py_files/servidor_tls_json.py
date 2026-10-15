import argparse
import hashlib
import json
import os
import socket
import ssl

# Directorio del script: ubicación predeterminada del certificado y la clave TLS.
DIRECTORY = os.path.dirname(os.path.abspath(__file__))


def parse_args():
    """Lee opciones de red, certificados y firmware a publicar."""
    parser = argparse.ArgumentParser(
        description="Servidor TLS de telemetría con distribución FOTA opcional."
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=3334)
    parser.add_argument("--cert", default=os.path.join(DIRECTORY, "server.crt"))
    parser.add_argument("--key", default=os.path.join(DIRECTORY, "server.key"))
    parser.add_argument("--firmware", help="Ruta al archivo hello_world.bin")
    parser.add_argument(
        "--version",
        help="Versión del firmware; debe coincidir con la versión dentro del binario",
    )
    args = parser.parse_args()

    # La ruta del firmware y su versión deben publicarse juntas para evitar
    # anunciar una imagen sin identificar o una versión sin imagen.
    if bool(args.firmware) != bool(args.version):
        parser.error("--firmware y --version deben usarse juntos.")
    # La ESP32 reserva un campo de 31 caracteres y espera una versión sin espacios.
    if args.version and (
        len(args.version) > 31
        or not all(c.isascii() and (c.isalnum() or c in "._-") for c in args.version)
    ):
        parser.error("--version debe usar hasta 31 caracteres ASCII alfanuméricos, ._-.")
    if not 1 <= args.port <= 65535:
        parser.error("--port debe estar entre 1 y 65535.")
    return args


def load_firmware(path):
    """Carga la imagen una vez al iniciar y calcula su huella SHA-256."""
    with open(path, "rb") as firmware_file:
        image = firmware_file.read()
    if not image:
        raise ValueError("El archivo de firmware está vacío.")
    digest = hashlib.sha256(image).hexdigest()
    return image, digest


def handle_client(connection, firmware, version, digest):
    """Procesa telemetría en líneas y, si corresponde, entrega una actualización."""
    with connection.makefile("rb") as reader:
        while True:
            # El protocolo separa mensajes JSON con salto de línea.
            line = reader.readline(512)
            if not line:
                return
            if not line.endswith(b"\n"):
                raise ValueError("La línea de telemetría excede el límite.")

            telemetry = json.loads(line.decode("utf-8"))
            if not isinstance(telemetry, dict):
                raise ValueError("La telemetría debe ser un objeto JSON.")
            print(f"Telemetría: {telemetry}")

            # Comparar con la versión informada por la RTU evita enviarle
            # repetidamente la misma imagen después de actualizar.
            device_version = telemetry.get("firmware_version")
            if firmware is not None and device_version != version:
                # Primero se anuncia el tamaño y el hash; a continuación van
                # exactamente esos bytes binarios, sin codificación de texto.
                header = f"OTA {version} {len(firmware)} {digest}\n".encode("ascii")
                connection.sendall(header)
                connection.sendall(firmware)
                acknowledgment = reader.readline(128)
                if acknowledgment == f"OK {version}\n".encode("ascii"):
                    print(f"OTA {version} confirmada por el dispositivo.")
                else:
                    print(f"OTA sin confirmación (respuesta: {acknowledgment!r}).")
                return

            # No se configuró una imagen o la RTU ya tiene la versión publicada.
            connection.sendall(b"NONE\n")


def main():
    """Prepara TLS y atiende conexiones entrantes de las RTU."""
    args = parse_args()
    firmware = None
    digest = None
    if args.firmware:
        # Mantener la imagen y su hash en memoria permite servirla a cada dispositivo.
        firmware, digest = load_firmware(args.firmware)
        print(
            f"Firmware preparado: versión {args.version}, "
            f"{len(firmware)} bytes, SHA-256 {digest}"
        )

    # El servidor presenta el certificado y la clave configurados al negociar TLS.
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certfile=args.cert, keyfile=args.key)

    # Escuchar en todas las interfaces por defecto; la RTU inicia la conexión saliente.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as raw_socket:
        raw_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        raw_socket.bind((args.host, args.port))
        raw_socket.listen()
        print(f"Servidor TLS escuchando en {args.host}:{args.port}...")

        while True:
            connection, address = raw_socket.accept()
            print(f"Conexión desde {address}; iniciando handshake TLS.")
            try:
                # Solo se procesa la aplicación después de establecer TLS.
                with context.wrap_socket(connection, server_side=True) as secure_connection:
                    print("Handshake TLS exitoso.")
                    handle_client(secure_connection, firmware, args.version, digest)
            except (OSError, ValueError) as error:
                # Un fallo de cliente no debe detener el servidor para los demás.
                print(f"Error en la conexión {address}: {error}")


if __name__ == "__main__":
    # Permite importar las funciones para pruebas sin iniciar el servidor.
    main()
