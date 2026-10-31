"""Вход, регистрация по приглашениям, версия протокола, хранение паролей, журнал."""
import hashlib

import support
from support import Conn, ServerTestCase, auth


class InviteTests(ServerTestCase):
    def test_registration_requires_an_invite(self):
        for invite in (None, "", "   ", "AAAA-BBBB-CCCC-DDDD"):
            with self.subTest(invite=invite):
                response = auth(self.server, "register", "newbie", "goodpass", invite)
                self.assertFalse(response["success"])
                self.assertIn("приглашен", response["message"])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM users"), 0)

    def test_invite_works_once_and_ignores_case_and_separators(self):
        code = self.server.invite()
        formatted = code.lower().replace("-", " ")
        self.assertTrue(auth(self.server, "register", "first", "goodpass", formatted)["success"])
        again = auth(self.server, "register", "second", "goodpass", code)
        self.assertFalse(again["success"])
        self.assertIn("приглашен", again["message"])

    def test_taken_nick_does_not_burn_the_invite(self):
        self.register("taken")
        code = self.server.invite()
        response = auth(self.server, "register", "taken", "goodpass", code)
        self.assertFalse(response["success"])
        self.assertIn("уже существует", response["message"])
        self.assertTrue(auth(self.server, "register", "free", "goodpass", code)["success"])

    def test_bad_invite_does_not_reveal_which_nicks_are_taken(self):
        self.register("taken")
        response = auth(self.server, "register", "taken", "goodpass", "ZZZZ-ZZZZ-ZZZZ-ZZZZ")
        self.assertIn("приглашен", response["message"])
        self.assertNotIn("существует", response["message"])

    def test_invite_is_recorded_as_used_by_the_new_user(self):
        code = self.server.invite()
        self.assertTrue(auth(self.server, "register", "owner", "goodpass", code)["success"])
        used_by = self.server.scalar("SELECT used_by FROM invites")
        self.assertEqual(used_by, self.server.scalar("SELECT id FROM users WHERE nickname='owner'"))


class ValidationTests(ServerTestCase):
    def test_invalid_nicks_and_passwords_are_rejected_without_burning_the_invite(self):
        code = self.server.invite()
        cases = [
            ("nick with leading space", " ab", "goodpass"),
            ("nick with trailing space", "ab ", "goodpass"),
            ("nick too short", "a", "goodpass"),
            ("nick too long", "x" * 33, "goodpass"),
            ("nick with newline", "bad\nnick", "goodpass"),
            ("nick with tab", "bad\tnick", "goodpass"),
            ("password too short", "okay1", "abc"),
            ("password too long", "okay2", "p" * 129),
        ]
        for name, nick, password in cases:
            with self.subTest(name):
                response = auth(self.server, "register", nick, password, code)
                self.assertFalse(response["success"], response)
                self.assertNotIn("приглашен", response["message"])
        # приглашение цело: подходящий (в том числе юникодный) ник регистрируется
        self.assertTrue(auth(self.server, "register", "Ёж😀", "goodpass", code)["success"])

    def test_boundary_lengths_are_accepted(self):
        self.assertTrue(auth(self.server, "register", "ab", "abcd", self.server.invite())["success"])
        self.assertTrue(auth(self.server, "register", "x" * 32, "p" * 128, self.server.invite())["success"])

    def test_malformed_requests_get_a_clean_refusal(self):
        for message in ({"type": "login", "protocol": support.PROTOCOL},
                        {"type": "login", "protocol": support.PROTOCOL, "nick": "a", "password": ""},
                        {"type": "login", "protocol": support.PROTOCOL, "nick": "x" * 100, "password": "pw"},
                        {"type": "login", "protocol": support.PROTOCOL, "nick": "a", "password": "p" * 129},
                        {"type": "register", "protocol": support.PROTOCOL, "nick": "", "password": "goodpass"}):
            with self.subTest(message=message):
                conn = Conn(self.server.port)
                self.conns.append(conn)
                conn.send(message)
                responses = conn.recv_type("auth_response", 1.0)
                self.assertEqual(len(responses), 1)
                self.assertFalse(responses[0]["success"])


class LoginTests(ServerTestCase):
    def test_failures_are_reported_to_the_client(self):
        # Регрессия: отказы раньше молча терялись, и клиент вечно ждал ответа
        self.register("known", "goodpass")
        wrong = auth(self.server, "login", "known", "wrongpass")
        self.assertFalse(wrong["success"])
        self.assertIn("пароль", wrong["message"].lower())
        unknown = auth(self.server, "login", "nobody", "goodpass")
        self.assertFalse(unknown["success"])
        self.assertIn("не существует", unknown["message"])

    def test_correct_password_logs_in_and_sends_the_user_list(self):
        self.register("known", "goodpass")
        conn, response = auth(self.server, "login", "known", "goodpass", keep=True)
        self.conns.append(conn)
        self.assertTrue(response["success"])
        self.assertEqual(len(conn.recv_type("user_list", 1.0)), 1)

    def test_password_is_case_sensitive(self):
        self.register("known", "GoodPass")
        self.assertFalse(auth(self.server, "login", "known", "goodpass")["success"])
        self.assertTrue(auth(self.server, "login", "known", "GoodPass")["success"])

    def test_second_login_of_the_same_user_keeps_working(self):
        # Окно входа и главное окно клиента подключаются отдельно
        self.register("known", "goodpass")
        first = self.login("known")
        second = self.login("known")
        self.assertFalse(first.closed or second.closed)

    def test_non_auth_requests_are_ignored_before_login(self):
        conn = Conn(self.server.port)
        self.conns.append(conn)
        conn.send({"type": "message", "to_nick": "x", "text": "hi", "client_msg_id": "c"})
        conn.send({"type": "get_offline_messages"})
        conn.send({"type": "search_user", "nick": "x"})
        self.assertEqual(conn.recv(0.8), [])


class ProtocolVersionTests(ServerTestCase):
    def test_old_or_missing_version_is_rejected_with_a_hint(self):
        for version in (None, 0):
            with self.subTest(version=version):
                response = auth(self.server, "login", "anyone", "goodpass", version=version)
                self.assertFalse(response["success"])
                self.assertIn("Приложение устарело", response["message"])
                self.assertIn("Обновите приложение", response["message"])

    def test_newer_client_is_told_the_server_is_outdated(self):
        response = auth(self.server, "login", "anyone", "goodpass", version=support.PROTOCOL + 1)
        self.assertFalse(response["success"])
        self.assertIn("Сервер устарел", response["message"])

    def test_version_is_checked_before_the_invite(self):
        code = self.server.invite()
        response = auth(self.server, "register", "newbie", "goodpass", code, version=None)
        self.assertIn("устарело", response["message"])
        self.assertTrue(auth(self.server, "register", "newbie", "goodpass", code)["success"],
                        "приглашение не должно сгорать из-за старого клиента")

    def test_every_auth_response_carries_the_server_version(self):
        self.register("known")
        for response in (auth(self.server, "login", "known", "goodpass"),
                         auth(self.server, "login", "known", "bad-pass"),
                         auth(self.server, "login", "anyone", "x", version=None)):
            self.assertEqual(response["protocol"], support.PROTOCOL)

    def test_failed_version_checks_do_not_count_towards_the_lockout(self):
        for _ in range(8):
            auth(self.server, "login", "anyone", "goodpass", version=None)
        self.register("known")
        self.assertTrue(auth(self.server, "login", "known", "goodpass")["success"])


class PasswordStorageTests(ServerTestCase):
    def stored(self, nick):
        return self.server.scalar("SELECT password_hash FROM users WHERE nickname=?", nick)

    def test_new_passwords_are_salted_pbkdf2(self):
        self.register("alice", "samepassword")
        self.register("bob", "samepassword")
        for nick in ("alice", "bob"):
            parts = self.stored(nick).split("$")
            self.assertEqual(parts[:2], ["pbkdf2-sha256", "600000"])
            self.assertEqual(len(parts), 4)
        self.assertNotEqual(self.stored("alice"), self.stored("bob"))
        self.assertNotIn("samepassword", self.stored("alice"))

    def test_legacy_sha256_hash_is_upgraded_after_a_successful_login(self):
        legacy = hashlib.sha256(b"oldpass").hexdigest()
        self.server.sql("INSERT INTO users (nickname, password_hash) VALUES ('old', ?), ('old2', ?)", legacy, legacy)
        self.assertFalse(auth(self.server, "login", "old", "wrong")["success"])
        self.assertEqual(self.stored("old"), legacy, "неверный пароль не должен менять хеш")
        self.assertTrue(auth(self.server, "login", "old", "oldpass")["success"])
        self.assertTrue(self.stored("old").startswith("pbkdf2-sha256$600000$"))
        self.assertTrue(auth(self.server, "login", "old", "oldpass")["success"])
        self.assertEqual(self.stored("old2"), legacy, "чужой хеш не тронут")


class LogRedactionTests(ServerTestCase):
    def test_passwords_and_invites_never_reach_the_log(self):
        code = self.server.invite()
        secret_new, secret_wrong = "S3cret-registration-pw", "S3cret-wrong-pw"
        auth(self.server, "register", "logged", secret_new, code)
        auth(self.server, "login", "logged", secret_wrong)
        auth(self.server, "login", "logged", secret_new)
        log = self.server.log()
        self.assertIn("logged", log, "журнал должен содержать сами события")
        for secret in (secret_new, secret_wrong, code, code.replace("-", "")):
            self.assertNotIn(secret, log)
        self.assertIn('"password":"***"', log.replace(" ", ""))


if __name__ == "__main__":
    import unittest
    unittest.main()
