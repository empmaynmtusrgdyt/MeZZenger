"""Серверные ограничения: размеры, скорость, блокировки по IP, число соединений, таймаут входа."""
import socket
import time
import unittest
import uuid

import support
from support import Conn, ServerTestCase, auth, count_of, send_message, sync


def cid():
    return str(uuid.uuid4())


def ipv6_loopback_available():
    try:
        with socket.socket(socket.AF_INET6) as s:
            s.bind(("::1", 0))
        return True
    except OSError:
        return False


class DataLimitTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        self.alice = self.user("alice")
        self.register("bob")

    def test_message_of_maximum_length_is_accepted(self):
        send_message(self.alice, "bob", "x" * 4000, cid())
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 1)

    def test_longer_message_is_rejected_with_error(self):
        send_message(self.alice, "bob", "x" * 4001, cid())
        got = sync(self.alice)
        self.assertEqual(count_of(got, "error"), 1)
        self.assertEqual(count_of(got, "message_ack"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 0)

    def test_overlong_client_msg_id_is_not_stored(self):
        send_message(self.alice, "bob", "hi", "a" * 65)
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 0)


class RateTests(ServerTestCase):
    def test_flood_is_throttled_and_the_bucket_refills(self):
        alice = self.user("alice")
        self.register("bob")
        for i in range(200):
            send_message(alice, "bob", f"m{i}", cid())
        # барьер здесь не годится: служебный запрос тоже упрётся в лимит
        acks = count_of(alice.recv(1.5), "message_ack")
        self.assertGreaterEqual(acks, 50)
        self.assertLess(acks, 120)
        time.sleep(1.2)
        alice.recv(0)
        send_message(alice, "bob", "after", cid())
        self.assertEqual(count_of(sync(alice), "message_ack"), 1)


class UnauthenticatedTests(ServerTestCase):
    def test_oversized_frame_before_login_closes_the_connection(self):
        conn = Conn(self.server.port)
        self.conns.append(conn)
        conn.send_raw((5000).to_bytes(4, "big") + b"x" * 100)
        conn._read_for(2)
        self.assertTrue(conn.closed)

    @unittest.skipIf(support.SKIP_SLOW, "медленный тест (MEZZENGER_SKIP_SLOW)")
    def test_idle_connection_is_dropped_after_the_login_timeout(self):
        conn = Conn(self.server.port)
        self.conns.append(conn)
        conn._read_for(25)
        self.assertFalse(conn.closed, "соединение закрыто слишком рано")
        conn._read_for(10)
        self.assertTrue(conn.closed)


class LockoutTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        self.register("alice")

    def fail_times(self, n, kind="login", **kw):
        for _ in range(n):
            response = auth(self.server, kind, "alice", "wrongpass", **kw)
            self.assertFalse(response["success"])

    def assertLocked(self, response):
        self.assertFalse(response["success"])
        self.assertIn("Слишком много", response["message"])
        self.assertIn("Повторите через", response["message"])

    def test_sixth_attempt_is_blocked_even_with_the_right_password(self):
        self.fail_times(5)
        self.assertLocked(auth(self.server, "login", "alice", "goodpass"))

    def test_registration_is_blocked_too(self):
        self.fail_times(5)
        self.assertLocked(auth(self.server, "register", "other", "goodpass", self.server.invite()))

    def test_success_does_not_reset_the_counter(self):
        self.fail_times(3)
        self.assertTrue(auth(self.server, "login", "alice", "goodpass")["success"])
        self.fail_times(2)
        self.assertLocked(auth(self.server, "login", "alice", "goodpass"))

    def test_guessing_invites_is_limited(self):
        for i in range(5):
            response = auth(self.server, "register", f"n{i}", "goodpass", "AAAA-BBBB-CCCC-DDDD")
            self.assertFalse(response["success"])
        self.assertLocked(auth(self.server, "register", "late", "goodpass", self.server.invite()))

    def test_restart_clears_the_lockout(self):
        self.fail_times(5)
        self.server.restart()
        self.assertTrue(auth(self.server, "login", "alice", "goodpass")["success"])

    def test_failed_version_checks_do_not_count(self):
        for _ in range(8):
            auth(self.server, "login", "alice", "goodpass", version=support.PROTOCOL + 1)
        self.assertTrue(auth(self.server, "login", "alice", "goodpass")["success"])

    @unittest.skipUnless(ipv6_loopback_available(), "нет IPv6 loopback")
    def test_other_address_is_not_affected(self):
        self.fail_times(5)
        self.assertLocked(auth(self.server, "login", "alice", "goodpass"))
        try:
            response = auth(self.server, "login", "alice", "goodpass", host="::1")
        except OSError:
            self.skipTest("сервер не слушает IPv6")
        self.assertTrue(response["success"])


class ConnectionLimitTests(ServerTestCase):
    def test_only_twenty_connections_per_address_stay_open(self):
        conns = []
        for _ in range(25):
            try:
                conns.append(Conn(self.server.port))
            except OSError:
                pass
        self.conns.extend(conns)
        for conn in conns:
            conn._read_for(0.3)
        self.assertEqual(len([c for c in conns if not c.closed]), 20)
        for conn in conns[:5]:
            conn.close()
        time.sleep(0.5)
        extra = Conn(self.server.port)
        self.conns.append(extra)
        extra.send({"type": "login", "nick": "nobody", "password": "goodpass", "protocol": support.PROTOCOL})
        self.assertEqual(len(extra.recv_type("auth_response", 2)), 1)


if __name__ == "__main__":
    unittest.main()
