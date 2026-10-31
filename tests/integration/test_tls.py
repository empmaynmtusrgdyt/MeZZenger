"""Шифрование канала и сертификат сервера."""
import os
import shutil
import ssl
import stat
import subprocess
import tempfile
import time
import unittest

import support
from support import Conn, Server, ServerTestCase


class TlsTests(ServerTestCase):
    def test_tls_12_or_newer_is_negotiated(self):
        conn = Conn(self.server.port)
        self.addCleanup(conn.close)
        self.assertIn(conn.s.version(), ("TLSv1.2", "TLSv1.3"))

    def test_tls_12_only_client_can_connect(self):
        conn = Conn(self.server.port, context=support.tls_context(max_version=ssl.TLSVersion.TLSv1_2))
        self.addCleanup(conn.close)
        self.assertEqual(conn.s.version(), "TLSv1.2")

    def test_plaintext_connection_gets_no_service(self):
        import json
        import socket
        import struct
        raw = socket.create_connection(("127.0.0.1", self.server.port), timeout=3)
        self.addCleanup(raw.close)
        body = json.dumps({"type": "login", "protocol": support.PROTOCOL, "nick": "x", "password": "y"}).encode()
        raw.sendall(struct.pack(">I", len(body)) + body)
        try:
            reply = raw.recv(4096)
        except (socket.timeout, OSError):
            reply = b""
        self.assertNotIn(b"auth_response", reply)
        self.assertNotIn("Login attempt", self.server.log())

    def test_served_certificate_is_the_one_from_the_files(self):
        conn = Conn(self.server.port)
        self.addCleanup(conn.close)
        der = conn.s.getpeercert(binary_form=True)
        import hashlib
        digest = hashlib.sha256(der).hexdigest().upper()
        served = ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))
        self.assertEqual(served, self.server.fingerprint)

    def test_server_prints_the_certificate_fingerprint(self):
        self.assertIn(self.server.fingerprint, self.server.log())


class CertificateFileTests(unittest.TestCase):
    def test_server_refuses_to_start_without_certificate(self):
        server = Server(with_cert=False)
        self.addCleanup(server.cleanup)
        process = subprocess.run([support.SERVER, "--port", str(server.port)], cwd=server.dir,
                                 capture_output=True, text=True, timeout=10)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("gen-cert.sh", process.stdout + process.stderr)

    def test_server_refuses_a_broken_certificate(self):
        server = Server()
        self.addCleanup(server.cleanup)
        with open(os.path.join(server.dir, "server.crt"), "w") as f:
            f.write("not a certificate")
        process = subprocess.run([support.SERVER, "--port", str(server.port)], cwd=server.dir,
                                 capture_output=True, text=True, timeout=10)
        self.assertNotEqual(process.returncode, 0)

    def test_server_accepts_custom_certificate_paths(self):
        server = Server(with_cert=False)
        self.addCleanup(server.cleanup)
        for name in ("server.crt", "server.key"):
            shutil.copy(os.path.join(support.cert_dir(), name), os.path.join(server.dir, "other-" + name))
        process = subprocess.Popen([support.SERVER, "--port", str(server.port), "--cert", "other-server.crt",
                                    "--key", "other-server.key"], cwd=server.dir,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (process.kill(), process.wait()))
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                Conn(server.port).close()
                return
            except (OSError, ssl.SSLError):
                time.sleep(0.1)
        self.fail("сервер с --cert/--key не запустился")


class GenCertScriptTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="mezzenger-gencert-")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.script = os.path.join(support.SOURCE_DIR, "scripts", "gen-cert.sh")

    def run_script(self, *args):
        return subprocess.run([self.script, *args], cwd=self.dir, capture_output=True, text=True)

    def test_creates_files_and_protects_the_key(self):
        result = self.run_script("myhost.example", "192.168.1.5")
        self.assertEqual(result.returncode, 0, result.stderr)
        mode = stat.S_IMODE(os.stat(os.path.join(self.dir, "server.key")).st_mode)
        self.assertEqual(mode & 0o077, 0, f"ключ доступен другим пользователям: {oct(mode)}")

        text = subprocess.run(["openssl", "x509", "-in", os.path.join(self.dir, "server.crt"), "-noout", "-text"],
                              capture_output=True, text=True).stdout
        for expected in ("DNS:localhost", "IP Address:127.0.0.1", "DNS:myhost.example", "IP Address:192.168.1.5"):
            self.assertIn(expected, text)
        self.assertIn(support.fingerprint_of_cert_file(os.path.join(self.dir, "server.crt")), result.stdout.upper().replace("SHA256 FINGERPRINT=", ""))

    def test_never_overwrites_existing_files(self):
        self.assertEqual(self.run_script().returncode, 0)
        key_path = os.path.join(self.dir, "server.key")
        with open(key_path, "rb") as f:
            before = f.read()
        again = self.run_script()
        self.assertNotEqual(again.returncode, 0)
        with open(key_path, "rb") as f:
            self.assertEqual(f.read(), before)


if __name__ == "__main__":
    unittest.main()
