"""Присутствие (presence) и смена пароля."""
import support
from support import ServerTestCase, auth, sync


class PresenceTests(ServerTestCase):
    def presence_of(self, conn, wait=0.8):
        """Всё присутствие за wait секунд; остальные сообщения отбрасываются."""
        return [m for m in conn.recv(wait) if m.get("type") == "presence"]

    def test_login_and_logout_are_broadcast(self):
        alice = self.user("alice")
        self.register("bob")
        self.presence_of(alice)
        bob = self.login("bob")
        seen = self.presence_of(alice)
        self.assertEqual([(m["nick"], m["online"]) for m in seen], [("bob", True)])
        self.assertGreater(seen[0]["id"], 0)

        bob.close()
        seen = self.presence_of(alice)
        self.assertEqual([(m["nick"], m["online"]) for m in seen], [("bob", False)])

    def test_new_registration_is_announced(self):
        alice = self.user("alice")
        self.register("newbie")
        self.presence_of(alice)
        self.login("newbie")
        seen = self.presence_of(alice)
        self.assertIn(("newbie", True), [(m["nick"], m["online"]) for m in seen])

    def test_second_session_of_the_same_user_does_not_mark_offline(self):
        alice = self.user("alice")
        self.register("bob")
        self.presence_of(alice)
        first = self.login("bob")
        self.presence_of(alice)
        self.login("bob")
        self.presence_of(alice)
        first.close()
        seen = self.presence_of(alice, 1.5)
        self.assertNotIn(False, [m["online"] for m in seen])

    def test_own_presence_is_not_sent_back(self):
        alice = self.user("alice")
        self.assertEqual(self.presence_of(alice, 0.8), [])


class ChangePasswordTests(ServerTestCase):
    def change(self, conn, old, new):
        conn.send({"type": "change_password", "old_password": old, "new_password": new})
        return conn.recv_type("change_password_response", 3)[0]

    def test_password_can_be_changed(self):
        alice = self.user("alice")
        response = self.change(alice, "goodpass", "betterpass")
        self.assertTrue(response["success"], response)
        self.assertFalse(auth(self.server, "login", "alice", "goodpass")["success"])
        self.assertTrue(auth(self.server, "login", "alice", "betterpass")["success"])
        self.assertTrue(self.server.scalar("SELECT password_hash FROM users").startswith("pbkdf2-sha256$"))

    def test_wrong_old_password_changes_nothing(self):
        alice = self.user("alice")
        response = self.change(alice, "wrongpass", "betterpass")
        self.assertFalse(response["success"])
        self.assertTrue(auth(self.server, "login", "alice", "goodpass")["success"])

    def test_new_password_must_fit_the_limits(self):
        alice = self.user("alice")
        for bad in ("abc", "x" * 129, ""):
            with self.subTest(length=len(bad)):
                self.assertFalse(self.change(alice, "goodpass", bad)["success"])
        self.assertTrue(auth(self.server, "login", "alice", "goodpass")["success"])

    def test_guessing_the_old_password_is_limited(self):
        alice = self.user("alice")
        for _ in range(5):
            self.assertFalse(self.change(alice, "wrongpass", "betterpass")["success"])
        response = self.change(alice, "goodpass", "betterpass")
        self.assertFalse(response["success"])
        self.assertIn("Слишком много", response["message"])

    def test_passwords_are_not_logged(self):
        alice = self.user("alice")
        self.change(alice, "goodpass", "uniquesecret77")
        self.assertNotIn("uniquesecret77", self.server.log())

    def test_requires_login(self):
        conn = support.Conn(self.server.port)
        self.conns.append(conn)
        conn.send({"type": "change_password", "old_password": "goodpass", "new_password": "betterpass"})
        self.assertEqual(conn.recv_type("change_password_response", 0.8), [])
