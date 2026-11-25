import argparse
import hashlib
import html
import json
import os
import socket
import ssl
import struct
import threading
from email.parser import BytesParser
from email.policy import default as email_policy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Directorio del script: ubicación predeterminada del certificado y la clave TLS.
DIRECTORY = os.path.dirname(os.path.abspath(__file__))
MAX_IMAGE_SIZE = 1024 * 1024
APP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFFSET = 16
APP_VERSION_SIZE = 32


class FirmwarePublication:
    """Guarda de forma sincronizada la imagen FOTA que se ofrece a los clientes."""

    def __init__(self, firmware=None, filename=None):
        self._lock = threading.Lock()
        self._image = None
        if firmware is not None:
            self.publish(firmware, filename)

    def publish(self, firmware, filename):
        version = extract_firmware_version(firmware)
        digest = hashlib.sha256(firmware).hexdigest()
        with self._lock:
            self._image = (firmware, version, digest, filename)

    def snapshot(self):
        with self._lock:
            return self._image


def valid_version(version):
    return (
        1 <= len(version) <= 31
        and all(c.isascii() and (c.isalnum() or c in "._-") for c in version)
    )


def extract_firmware_version(image):
    """Obtiene la versión del descriptor de aplicación ESP-IDF en un .bin."""
    if len(image) < 24 or image[0] != APP_IMAGE_MAGIC:
        raise ValueError("El archivo no parece una imagen ESP válida.")

    segment_count = image[1]
    if not 1 <= segment_count <= 16:
        raise ValueError("La imagen ESP contiene una cantidad de segmentos inválida.")

    offset = 24
    for _ in range(segment_count):
        if offset + 8 > len(image):
            raise ValueError("La imagen ESP está truncada en una cabecera de segmento.")
        _, segment_size = struct.unpack_from("<II", image, offset)
        offset += 8
        segment_end = offset + segment_size
        if segment_end > len(image):
            raise ValueError("La imagen ESP está truncada en un segmento.")

        segment = image[offset:segment_end]
        magic = struct.pack("<I", APP_DESC_MAGIC)
        magic_offset = segment.find(magic)
        while magic_offset >= 0:
            version_start = magic_offset + APP_DESC_VERSION_OFFSET
            version_end = version_start + APP_VERSION_SIZE
            if version_end <= len(segment):
                version_field = segment[version_start:version_end]
                version_bytes = version_field.split(b"\0", 1)[0]
                try:
                    version = version_bytes.decode("ascii")
                except UnicodeDecodeError:
                    version = ""
                if valid_version(version):
                    return version
            magic_offset = segment.find(magic, magic_offset + 1)
        offset = segment_end

    raise ValueError("No se encontró una versión válida en el descriptor ESP-IDF.")


def parse_args():
    """Lee opciones de red, certificados y firmware a publicar."""
    parser = argparse.ArgumentParser(
        description="Servidor TLS de telemetría con distribución FOTA."
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=3334)
    parser.add_argument(
        "--control-port",
        type=int,
        default=8080,
        help="Puerto de la interfaz web local (solo accesible desde esta PC)",
    )
    parser.add_argument("--cert", default=os.path.join(DIRECTORY, "server.crt")    )
    parser.add_argument("--key", default=os.path.join(DIRECTORY, "server.key"))
    parser.add_argument("--firmware", help="Ruta al archivo .bin a publicar al iniciar")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("--port debe estar entre 1 y 65535.")
    if not 1 <= args.control_port <= 65535:
        parser.error("--control-port debe estar entre 1 y 65535.")
    return args


def load_firmware(path):
    """Carga una imagen y obtiene su versión embebida y huella SHA-256."""
    with open(path, "rb") as firmware_file:
        image = firmware_file.read()
    if not image:
        raise ValueError("El archivo de firmware está vacío.")
    if len(image) > MAX_IMAGE_SIZE:
        raise ValueError(f"El firmware supera el máximo de {MAX_IMAGE_SIZE} bytes.")
    version = extract_firmware_version(image)
    digest = hashlib.sha256(image).hexdigest()
    return image, version, digest


def make_control_handler(publication):
    class FirmwareControlHandler(BaseHTTPRequestHandler):
        def _is_local_request(self):
            host = self.headers.get("Host", "").rsplit(":", 1)[0].strip("[]")
            return (
                self.client_address[0] == "127.0.0.1"
                and host in ("127.0.0.1", "localhost")
            )

        def _send_html(self, status, content):
            body = content.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _redirect_home(self):
            self.send_response(303)
            self.send_header("Location", "/")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            if not self._is_local_request():
                self.send_error(403, "La interfaz solo acepta conexiones locales.")
                return
            if self.path != "/":
                self.send_error(404)
                return

            current = publication.snapshot()
            if current is None:
                status = "<p>No hay firmware publicado. La RTU recibirá <code>NONE</code>.</p>"
            else:
                _, version, digest, filename = current
                status = (
                    "<p>Firmware publicado: <strong>"
                    f"{html.escape(filename)}</strong>, versión "
                    f"<strong>{html.escape(version)}</strong>.</p>"
                    f"<p>SHA-256: <code>{digest}</code></p>"
                )

            page = f"""<!doctype html>
<html lang="es"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Control FOTA RTU</title>
<style>
body{{font:16px Arial,sans-serif;max-width:680px;margin:40px auto;padding:0 18px;background:#f4f6fa;color:#172033}}
main{{background:white;padding:24px;border-radius:10px;box-shadow:0 2px 12px #0001}}
label{{display:block;margin:18px 0 6px}}input,button{{font:inherit;padding:10px;width:100%;box-sizing:border-box}}
button{{margin-top:14px;background:#0759a5;color:white;border:0;border-radius:5px;cursor:pointer}}
code{{overflow-wrap:anywhere}}
</style>
<main><h1>Control FOTA RTU</h1>{status}
<form action="/publish" method="post" enctype="multipart/form-data">
<label for="firmware">Archivo .bin</label>
<input id="firmware" name="firmware" type="file" accept=".bin" required>
<button type="submit">Publicar firmware</button></form>
<p>La versión se detecta automáticamente desde el binario. El servidor responde
<code>NONE</code> si un dispositivo ya tiene esa versión y ofrece el firmware si difiere.</p>
</main></html>"""
            self._send_html(200, page)

        def do_POST(self):
            if not self._is_local_request():
                self.send_error(403, "La interfaz solo acepta conexiones locales.")
                return
            if self.path != "/publish":
                self.send_error(404)
                return

            content_type = self.headers.get("Content-Type", "")
            if not content_type.lower().startswith("multipart/form-data;"):
                self.send_error(400, "Se esperaba un formulario multipart.")
                return
            try:
                content_length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                self.send_error(411, "Falta una longitud de contenido válida.")
                return
            if content_length <= 0:
                self.send_error(400, "El formulario está vacío.")
                return
            if content_length > MAX_IMAGE_SIZE + 65536:
                self.send_error(413, "El formulario supera el tamaño permitido.")
                return

            body = self.rfile.read(content_length)
            if len(body) != content_length:
                self.send_error(400, "No se recibió el formulario completo.")
                return

            mime_message = BytesParser(policy=email_policy).parsebytes(
                b"Content-Type: "
                + content_type.encode("ascii", errors="replace")
                + b"\r\nMIME-Version: 1.0\r\n\r\n"
                + body
            )
            fields = {}
            if mime_message.is_multipart():
                for part in mime_message.iter_parts():
                    name = part.get_param("name", header="content-disposition")
                    if name == "firmware":
                        fields[name] = part

            firmware_part = fields.get("firmware")
            if firmware_part is None:
                self.send_error(400, "Selecciona un archivo .bin.")
                return

            filename = firmware_part.get_filename() or ""
            firmware = firmware_part.get_payload(decode=True)
            if not filename.lower().endswith(".bin"):
                self.send_error(400, "El archivo seleccionado debe tener extensión .bin.")
                return
            if not firmware:
                self.send_error(400, "El archivo de firmware está vacío.")
                return
            if len(firmware) > MAX_IMAGE_SIZE:
                self.send_error(413, f"El firmware supera el máximo de {MAX_IMAGE_SIZE} bytes.")
                return

            filename = os.path.basename(filename)
            try:
                publication.publish(firmware, filename)
            except ValueError as error:
                self.send_error(400, str(error))
                return
            _, version, digest, _ = publication.snapshot()
            print(
                f"Firmware publicado desde la interfaz: versión {version}, "
                f"{len(firmware)} bytes, SHA-256 {digest}"
            )
            self._redirect_home()

        def log_message(self, format_string, *args):
            print(f"Interfaz FOTA: {format_string % args}")

    return FirmwareControlHandler


def handle_client(connection, publication):
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

            current = publication.snapshot()
            device_version = telemetry.get("firmware_version")
            if current is not None:
                firmware, version, digest, _ = current
            else:
                firmware = version = digest = None

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


def handle_tls_connection(context, connection, address, publication):
    """Completa TLS y atiende un cliente sin bloquear la aceptación de otros."""
    try:
        connection.settimeout(10)
        with context.wrap_socket(connection, server_side=True) as secure_connection:
            secure_connection.settimeout(None)
            print(f"Handshake TLS exitoso con {address}.")
            handle_client(secure_connection, publication)
    except (OSError, ValueError) as error:
        print(f"Error en la conexión {address}: {error}")
    finally:
        connection.close()


def main():
    """Prepara TLS y atiende conexiones entrantes de las RTU."""
    args = parse_args()
    firmware = None
    if args.firmware:
        # Mantener la imagen y su hash en memoria permite servirla a cada dispositivo.
        firmware, version, digest = load_firmware(args.firmware)
        filename = os.path.basename(args.firmware)
    else:
        version = digest = filename = None
    publication = FirmwarePublication(firmware, filename)
    if firmware is not None:
        print(f"Firmware preparado: versión {version}, {len(firmware)} bytes, SHA-256 {digest}")

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

        control_server = ThreadingHTTPServer(
            ("127.0.0.1", args.control_port),
            make_control_handler(publication),
        )
        control_thread = threading.Thread(
            target=control_server.serve_forever,
            name="firmware-control",
            daemon=True,
        )
        control_thread.start()
        print(f"Interfaz local: http://127.0.0.1:{args.control_port}")

        try:
            while True:
                connection, address = raw_socket.accept()
                print(f"Conexión desde {address}; iniciando handshake TLS.")
                threading.Thread(
                    target=handle_tls_connection,
                    args=(context, connection, address, publication),
                    name=f"tls-client-{address[0]}-{address[1]}",
                    daemon=True,
                ).start()
        finally:
            control_server.shutdown()
            control_server.server_close()


if __name__ == "__main__":
    # Permite importar las funciones para pruebas sin iniciar el servidor.
    main()
