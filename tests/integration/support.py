"""Общие средства интеграционных тестов: настоящий сервер во временном каталоге,
клиент протокола поверх TLS и запуск обвязки настоящего окна клиента."""
import hashlib
import json
import os
import re
import shutil
import socket
import sqlite3
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest

SERVER = os.environ.get("MEZZENGER_SERVER", "")
HARNESS = os.environ.get("MEZZENGER_HARNESS", "")
SOURCE_DIR = os.environ.get("MEZZENGER_SOURCE_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
SKIP_SLOW = os.environ.get("MEZZENGER_SKIP_SLOW") == "1"


def protocol_version():
    """Версия протокола берётся из исходников, чтобы тесты не отставали от неё."""
    with open(os.path.join(SOURCE_DIR, "common", "Protocol.h"), encoding="utf-8") as f:
        return int(re.search(r"constexpr int Version = (\d+);", f.read()).group(1))


PROTOCOL = protocol_version()

_cert_dir = None


def cert_dir():
    """Один сертификат на весь запуск тестов: создаётся настоящим скриптом scripts/gen-cert.sh."""
    global _cert_dir
    if _cert_dir is None:
        _cert_dir = tempfile.mkdtemp(prefix="mezzenger-cert-")
        subprocess.run([os.path.join(SOURCE_DIR, "scripts", "gen-cert.sh")], cwd=_cert_dir,
                       check=True, capture_output=True)
    return _cert_dir


def fingerprint_of_cert_file(path):
    with open(path, encoding="ascii") as f:
        der = ssl.PEM_cert_to_DER_cert(f.read())
    digest = hashlib.sha256(der).hexdigest().upper()
    return ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """Настоящий mezzenger_server со своей базой, портом и журналом."""

    def __init__(self, with_cert=True):
        assert SERVER, "не задан MEZZENGER_SERVER (запускайте тесты через ctest)"
        self.dir = tempfile.mkdtemp(prefix="mezzenger-srv-")
        self.port = free_port()
        self.process = None
        self.log_path = os.path.join(self.dir, "server.log")
        if with_cert:
            for name in ("server.crt", "server.key"):
                shutil.copy(os.path.join(cert_dir(), name), os.path.join(self.dir, name))

    @property
    def fingerprint(self):
        return fingerprint_of_cert_file(os.path.join(self.dir, "server.crt"))

    @property
    def db_path(self):
        return os.path.join(self.dir, "server.db")

    def start(self):
        log = open(self.log_path, "ab")
        self.process = subprocess.Popen([SERVER, "--port", str(self.port)], cwd=self.dir, stdout=log, stderr=log)
        log.close()
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("сервер завершился при запуске:\n" + self.log())
            try:
                Conn(self.port).close()
                return self
            except (OSError, ssl.SSLError):
                time.sleep(0.1)
        raise RuntimeError("сервер не начал принимать подключения:\n" + self.log())

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def restart(self):
        self.stop()
        return self.start()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def log(self):
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def invite(self):
        out = subprocess.run([SERVER, "--new-invite"], cwd=self.dir, capture_output=True, text=True, check=True)
        return out.stdout.strip()

    def sql(self, query, *args):
        db = sqlite3.connect(self.db_path, timeout=10)
        try:
            rows = db.execute(query, args).fetchall()
            db.commit()
            return rows
        finally:
            db.close()

    def sql_many(self, query, rows):
        db = sqlite3.connect(self.db_path, timeout=10)
        try:
            db.executemany(query, rows)
            db.commit()
        finally:
            db.close()

    def scalar(self, query, *args):
        rows = self.sql(query, *args)
        return rows[0][0] if rows else None


def tls_context(max_version=None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if max_version:
        ctx.maximum_version = max_version
    return ctx


class Conn:
    """Клиент протокола: TLS + длина (4 байта) + JSON."""

    def __init__(self, port, host="127.0.0.1", context=None):
        family = socket.AF_INET6 if ":" in host else socket.AF_INET
        raw = socket.socket(family)
        raw.settimeout(5)
        try:
            raw.connect((host, port))
            self.s = (context or tls_context()).wrap_socket(raw, server_hostname="mezzenger")
        except BaseException:
            raw.close()
            raise
        self.s.settimeout(0.2)
        self.buf = b""
        self.pending = []
        self.closed = False

    def send(self, obj):
        data = json.dumps(obj).encode()
        self.s.sendall(struct.pack(">I", len(data)) + data)

    def send_raw(self, data):
        self.s.sendall(data)

    def _read_for(self, seconds, stop_when=None):
        """Читает с сокета до истечения времени (или пока stop_when не вернёт True)."""
        out, end = [], time.time() + seconds
        while time.time() < end:
            try:
                data = self.s.recv(65536)
            except (socket.timeout, ssl.SSLWantReadError):
                continue
            except (ssl.SSLError, OSError):
                self.closed = True
                break
            if data == b"":
                self.closed = True
                break
            self.buf += data
            while len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if len(self.buf) < 4 + n:
                    break
                out.append(json.loads(self.buf[4:4 + n]))
                self.buf = self.buf[4 + n:]
            if out and stop_when and stop_when(out):
                break
        return out

    def read_frame(self, timeout=5):
        """Ровно один кадр; остальное остаётся в буфере (после file_info идут сырые байты)."""
        end = time.time() + timeout
        while time.time() < end:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    frame = json.loads(self.buf[4:4 + n])
                    self.buf = self.buf[4 + n:]
                    return frame
            try:
                data = self.s.recv(65536)
            except (socket.timeout, ssl.SSLWantReadError):
                continue
            except (ssl.SSLError, OSError):
                self.closed = True
                return None
            if data == b"":
                self.closed = True
                return None
            self.buf += data
        return None

    def read_exact(self, size, timeout=30):
        end = time.time() + timeout
        while len(self.buf) < size and time.time() < end:
            try:
                data = self.s.recv(1 << 20)
            except (socket.timeout, ssl.SSLWantReadError):
                continue
            except (ssl.SSLError, OSError):
                self.closed = True
                break
            if data == b"":
                self.closed = True
                break
            self.buf += data
        out, self.buf = self.buf[:size], self.buf[size:]
        return out

    def recv(self, wait=0.6):
        """Все сообщения, пришедшие за wait секунд (плюс не прочитанные ранее)."""
        out, self.pending = self.pending, []
        return out + self._read_for(wait)

    def recv_type(self, kind, wait=0.8):
        """Сообщения нужного типа. Возвращается сразу, как только такое пришло; остальные
        сообщения остаются для следующих чтений. Если ждём отсутствия ответа, ждёт все wait секунд."""
        messages = self.pending + self._read_for(wait, lambda got: any(m.get("type") == kind for m in self.pending + got))
        self.pending = [m for m in messages if m.get("type") != kind]
        return [m for m in messages if m.get("type") == kind]

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def auth(server, kind, nick, password, invite=None, version=PROTOCOL, host="127.0.0.1", keep=False):
    """Вход или регистрация. Возвращает auth_response (или (соединение, ответ), если keep)."""
    conn = Conn(server.port, host)
    message = {"type": kind, "nick": nick, "password": password}
    if version is not None:
        message["protocol"] = version
    if invite is not None:
        message["invite"] = invite
    conn.send(message)
    responses = conn.recv_type("auth_response", 1.0)
    response = responses[0] if responses else None
    if keep:
        return conn, response
    conn.close()
    return response


class ServerTestCase(unittest.TestCase):
    """Каждый тест получает чистый сервер: своя база, свои счётчики блокировок."""

    def setUp(self):
        self.server = Server().start()
        self.addCleanup(self.server.cleanup)
        self.conns = []
        self.addCleanup(self._close_conns)

    def _close_conns(self):
        for conn in self.conns:
            conn.close()

    def register(self, nick, password="goodpass"):
        response = auth(self.server, "register", nick, password, self.server.invite())
        self.assertTrue(response and response["success"], f"регистрация {nick}: {response}")

    def login(self, nick, password="goodpass"):
        conn, response = auth(self.server, "login", nick, password, keep=True)
        self.assertTrue(response and response["success"], f"вход {nick}: {response}")
        conn.recv_type("user_list", 3)
        conn.recv_type("group_list", 3)
        self.conns.append(conn)
        return conn

    def user(self, nick, password="goodpass"):
        """Зарегистрировать и войти."""
        self.register(nick, password)
        return self.login(nick, password)


def sync(conn, timeout=5, presence=False):
    """Барьер: сервер обрабатывает сообщения одного соединения по порядку, поэтому ответ на служебный
    запрос приходит после всего, что было отправлено раньше. Возвращает всё, что пришло до барьера.
    Чтобы увидеть доставку другому пользователю, сначала синхронизируйте отправителя, потом получателя."""
    conn.send({"type": "search_user", "nick": ""})
    messages, end = conn.pending, time.time() + timeout
    conn.pending = []
    while time.time() < end:
        barrier = [m for m in messages if m.get("type") == "search_user_response"]
        if barrier:
            messages.remove(barrier[0])
            # Рассылка присутствия идёт вперемешку с остальным; тестам доставки она не нужна
            return messages if presence else [m for m in messages if m.get("type") != "presence"]
        messages += conn._read_for(0.5)
    raise AssertionError("сервер не ответил на служебный запрос")


def count_of(messages, kind):
    return len([m for m in messages if m.get("type") == kind])


def request_upload(conn, name, size, ref="r1"):
    conn.send({"type": "file_upload_request", "file_name": name, "file_size": size, "ref": ref})
    return conn.recv_type("file_ticket", 3)[0]


def upload_file(server, conn, name, data, close_after=None, extra=b""):
    """Загрузка целиком: разрешение по основному каналу, байты по отдельному соединению.
    Возвращает (ticket, file_result); close_after - сколько байт отправить до обрыва."""
    ticket = request_upload(conn, name, len(data))
    if not ticket["success"]:
        return ticket, None
    transfer = Conn(server.port)
    try:
        transfer.send({"type": "file_put", "token": ticket["token"]})
        if close_after is not None:
            transfer.send_raw(data[:close_after])
            return ticket, None
        transfer.send_raw(data + extra)
        return ticket, transfer.read_frame(10)
    finally:
        transfer.close()


def download_file(server, conn, file_id):
    """Скачивание: возвращает (ticket, bytes или None)."""
    conn.send({"type": "file_download_request", "file_id": file_id})
    ticket = conn.recv_type("file_ticket", 3)[0]
    if not ticket["success"]:
        return ticket, None
    transfer = Conn(server.port)
    try:
        transfer.send({"type": "file_get", "token": ticket["token"]})
        info = transfer.read_frame(10)
        if info is None or info.get("type") != "file_info":
            return ticket, None
        return ticket, transfer.read_exact(info["size"])
    finally:
        transfer.close()


def send_file_message(conn, to_nick, file_id, client_msg_id, text=""):
    conn.send({"type": "message", "to_nick": to_nick, "text": text, "client_msg_id": client_msg_id, "file_id": file_id})


def send_message(conn, to_nick, text, client_msg_id):
    conn.send({"type": "message", "to_nick": to_nick, "text": text, "client_msg_id": client_msg_id})


def fetch_offline(conn, wait=0.8):
    conn.send({"type": "get_offline_messages"})
    batches = conn.recv_type("message_history", wait)
    return batches[0]["messages"] if batches else None


# ---------------------------------------------------------------- обвязка настоящего клиента

class HarnessRun:
    """Запущенная обвязка клиента (client_harness): события читаются из её вывода."""

    def __init__(self, server, nick, mode="login", password="pass1234", invite=None, pin="fingerprint",
                 answer="yes", script=None, duration=6000, workdir=None, settings_dir=None):
        assert HARNESS, "не задан MEZZENGER_HARNESS (запускайте тесты через ctest)"
        self.workdir = workdir or tempfile.mkdtemp(prefix="mezzenger-cli-")
        self.settings_dir = settings_dir or tempfile.mkdtemp(prefix="mezzenger-settings-")
        self.own_dirs = workdir is None
        args = [HARNESS, "--nick", nick, "--mode", mode, "--port", str(server.port), "--password", password,
                "--answer", answer, "--duration", str(duration), "--settings-dir", self.settings_dir]
        if invite is not None:
            args += ["--invite", invite]
        if pin == "fingerprint":
            pin = server.fingerprint
        if pin is not None:
            args += ["--pin", pin]
        if script:
            args += ["--script", script]
        self.out_path = os.path.join(self.workdir, "harness.out")
        self.out = open(self.out_path, "wb")
        self.started = time.time()
        self.process = subprocess.Popen(args, cwd=self.workdir, stdout=self.out, stderr=subprocess.DEVNULL)

    def events(self):
        with open(self.out_path, encoding="utf-8", errors="replace") as f:
            result = []
            for line in f:
                match = re.match(r"HARNESS (\S+?): ?(.*)$", line.rstrip("\n"))
                if match:
                    result.append((match.group(1), match.group(2)))
            return result

    def of(self, kind):
        return [data for k, data in self.events() if k == kind]

    def wait_for(self, kind, contains=None, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for data in self.of(kind):
                if contains is None or contains in data:
                    return data
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        return None

    def finish(self, timeout=40):
        try:
            self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.out.close()
        return self

    def end_state(self):
        """Итоговая строка end: login_visible=.. main_visible=.. и т. д. в виде словаря."""
        ends = self.of("end")
        if not ends:
            return None
        state = {}
        for part in re.finditer(r"(\w+)=(.*?)(?= \w+=|$)", ends[-1]):
            state[part.group(1)] = part.group(2)
        return state

    @property
    def client_db(self):
        return os.path.join(self.workdir, "mezzenger_client.db")

    def client_rows(self, query, *args):
        db = sqlite3.connect(self.client_db, timeout=10)
        try:
            return db.execute(query, args).fetchall()
        finally:
            db.close()

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        if not self.out.closed:
            self.out.close()
        if self.own_dirs:
            shutil.rmtree(self.workdir, ignore_errors=True)
            shutil.rmtree(self.settings_dir, ignore_errors=True)
