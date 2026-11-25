import hashlib
import http.client
import io
import os
import struct
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from unittest.mock import patch

import servidor_tls_json as server


def make_firmware(version):
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, server.APP_DESC_MAGIC)
    encoded_version = version.encode("ascii") + b"\0"
    descriptor[server.APP_DESC_VERSION_OFFSET:
               server.APP_DESC_VERSION_OFFSET + len(encoded_version)] = encoded_version

    image = bytearray(24)
    image[0] = server.APP_IMAGE_MAGIC
    image[1] = 1
    image.extend(struct.pack("<II", 0x3F400020, len(descriptor)))
    image.extend(descriptor)
    return bytes(image)


class FirmwareVersionTests(unittest.TestCase):
    def test_extracts_embedded_version_and_publishes_it(self):
        image = make_firmware("1.2.3")
        self.assertEqual(server.extract_firmware_version(image), "1.2.3")

        publication = server.FirmwarePublication(image, "firmware.bin")
        published_image, version, digest, filename = publication.snapshot()
        self.assertEqual(published_image, image)
        self.assertEqual(version, "1.2.3")
        self.assertEqual(digest, hashlib.sha256(image).hexdigest())
        self.assertEqual(filename, "firmware.bin")

    def test_rejects_non_esp_or_unversioned_images(self):
        with self.assertRaises(ValueError):
            server.extract_firmware_version(b"not an ESP image")
        with self.assertRaises(ValueError):
            server.extract_firmware_version(make_firmware("invalid version"))

    def test_load_firmware_extracts_version(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "firmware.bin")
            image = make_firmware("2.0.0")
            with open(path, "wb") as firmware_file:
                firmware_file.write(image)

            loaded_image, version, digest = server.load_firmware(path)

        self.assertEqual(loaded_image, image)
        self.assertEqual(version, "2.0.0")
        self.assertEqual(digest, hashlib.sha256(image).hexdigest())


class FirmwareControlInterfaceTests(unittest.TestCase):
    def test_upload_needs_only_the_bin_and_publishes_embedded_version(self):
        image = make_firmware("3.1.0")
        publication = server.FirmwarePublication()
        http_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), server.make_control_handler(publication)
        )
        thread = threading.Thread(target=http_server.serve_forever)
        thread.start()
        try:
            boundary = "firmware-test-boundary"
            body = (
                f"--{boundary}\r\n"
                'Content-Disposition: form-data; name="firmware"; filename="new.bin"\r\n'
                "Content-Type: application/octet-stream\r\n\r\n"
            ).encode("ascii") + image + f"\r\n--{boundary}--\r\n".encode("ascii")
            connection = http.client.HTTPConnection(
                "127.0.0.1", http_server.server_address[1]
            )
            connection.request(
                "POST",
                "/publish",
                body=body,
                headers={
                    "Content-Type": f"multipart/form-data; boundary={boundary}",
                    "Content-Length": str(len(body)),
                },
            )
            response = connection.getresponse()
            response.read()
            self.assertEqual(response.status, 303)
            self.assertEqual(publication.snapshot()[1], "3.1.0")

            connection.request("GET", "/")
            page_response = connection.getresponse()
            page = page_response.read().decode("utf-8")
            self.assertEqual(page_response.status, 200)
            self.assertIn("3.1.0", page)
            self.assertIn('name="firmware"', page)
            self.assertNotIn('name="version"', page)
            connection.close()
        finally:
            http_server.shutdown()
            http_server.server_close()
            thread.join()


class FirmwareProtocolTests(unittest.TestCase):
    class FakeConnection:
        def __init__(self, inbound):
            self.reader = io.BytesIO(inbound)
            self.sent = bytearray()

        def makefile(self, _mode):
            return self.reader

        def sendall(self, data):
            self.sent.extend(data)

    def test_same_or_missing_firmware_gets_none(self):
        image = make_firmware("1.0")
        publication = server.FirmwarePublication(image, "firmware.bin")
        for version in ("1.0", "0.9"):
            connection = self.FakeConnection(
                f'{{"sensor_id":"ESP32-A","firmware_version":"{version}"}}\n'.encode()
            )
            if version == "0.9":
                empty_publication = server.FirmwarePublication()
                server.handle_client(connection, empty_publication)
            else:
                server.handle_client(connection, publication)
            self.assertEqual(connection.sent, b"NONE\n")

    def test_different_version_receives_firmware_and_waits_for_ack(self):
        image = make_firmware("2.0")
        publication = server.FirmwarePublication(image, "firmware.bin")
        digest = hashlib.sha256(image).hexdigest()
        inbound = b'{"sensor_id":"ESP32-A","firmware_version":"1.0"}\nOK 2.0\n'
        connection = self.FakeConnection(inbound)

        with patch("builtins.print") as print_mock:
            server.handle_client(connection, publication)

        print_mock.assert_any_call(
            "Telemetría: {'sensor_id': 'ESP32-A', 'firmware_version': '1.0'}"
        )
        self.assertEqual(
            bytes(connection.sent),
            f"OTA 2.0 {len(image)} {digest}\n".encode("ascii") + image,
        )


if __name__ == "__main__":
    unittest.main()
