"""Настоящий клиент (LoginWindow -> MainWindow) под управлением client_harness, без показа окон."""
import time
import unittest

import support
from support import HarnessRun, ServerTestCase, auth


class ClientGuiTests(ServerTestCase):
    def run_client(self, nick, **kw):
        run = HarnessRun(self.server, nick, **kw)
        self.addCleanup(run.cleanup)
        return run

    def register_via_gui(self, nick="gui", **kw):
        run = self.run_client(nick, mode="reg", invite=self.server.invite(), duration=3000, **kw)
        self.assertIsNotNone(run.wait_for("login-successful"), run.events())
        return run.finish()

    def test_registration_with_invite_logs_in(self):
        run = self.register_via_gui()
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM users WHERE nickname = 'gui'"), 1)
        self.assertEqual(run.end_state()["main_visible"], "1")

    def test_registration_without_invite_is_refused_locally(self):
        run = self.run_client("gui", mode="reg", duration=2500).finish()
        self.assertIsNotNone(run.wait_for("dialog", "код приглашения") or
                             next((d for d in run.of("dialog") if "код приглашения" in d), None), run.events())
        self.assertEqual(run.of("login-successful"), [])
        self.assertNotIn("register", self.server.log())

    def test_wrong_password_and_unknown_user_show_a_dialog_and_keep_the_login_window(self):
        self.register("alice", "pass1234")
        for nick, password in (("alice", "wrongpass"), ("ghost", "pass1234")):
            with self.subTest(nick=nick):
                run = self.run_client(nick, password=password, duration=2500).finish()
                self.assertEqual(run.of("login-successful"), [])
                self.assertTrue(run.of("dialog"), run.events())
                state = run.end_state()
                self.assertEqual(state["login_visible"], "1")
                self.assertEqual(state["buttons_enabled"], "1")

    def test_first_connection_asks_to_trust_the_fingerprint(self):
        self.register("alice", "pass1234")
        run = self.run_client("alice", pin=None, answer="yes", duration=3000).finish()
        self.assertTrue(any(self.server.fingerprint.lower() in d.lower().replace(":", "") or
                            self.server.fingerprint.lower() in d.lower() for d in run.of("dialog")), run.events())
        self.assertEqual(len(run.of("login-successful")), 1)
        self.assertTrue(run.end_state()["pin"])

    def test_declining_the_fingerprint_blocks_login(self):
        self.register("alice", "pass1234")
        run = self.run_client("alice", pin=None, answer="no", duration=2500).finish()
        self.assertEqual(run.of("login-successful"), [])
        state = run.end_state()
        self.assertEqual(state["buttons_enabled"], "1")
        self.assertEqual(state["pin"], "")

    def test_known_fingerprint_gives_no_prompt(self):
        self.register("alice", "pass1234")
        run = self.run_client("alice", duration=2500).finish()
        self.assertEqual(len(run.of("login-successful")), 1)
        self.assertEqual(run.of("dialog"), [])

    def test_changed_fingerprint_is_flagged(self):
        self.register("alice", "pass1234")
        fake = "00" * 32
        no = self.run_client("alice", pin=fake, answer="no", duration=2500).finish()
        self.assertTrue(any("изменился" in d for d in no.of("dialog")), no.events())
        self.assertEqual(no.of("login-successful"), [])
        yes = self.run_client("alice", pin=fake, answer="yes", duration=3000).finish()
        self.assertEqual(len(yes.of("login-successful")), 1)
        self.assertNotEqual(yes.end_state()["pin"], fake)

    def test_lockout_message_reaches_the_user(self):
        self.register("alice", "pass1234")
        for _ in range(5):
            auth(self.server, "login", "alice", "wrongpass")
        run = self.run_client("alice", duration=2500).finish()
        self.assertTrue(any("Слишком много" in d for d in run.of("dialog")), run.events())
        self.assertEqual(run.of("login-successful"), [])

    def test_sent_message_is_acked_and_stored_once(self):
        self.register("alice", "pass1234")
        self.register("bob", "pass1234")
        run = self.run_client("alice", script="1500:search:bob,2500:send:hello,4000:dump", duration=5500).finish()
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE message = 'hello'"), 1)
        rows = run.client_rows("SELECT status, server_id FROM messages WHERE message = 'hello'")
        self.assertEqual(len(rows), 1, run.events())
        self.assertEqual(rows[0][0], 1)
        self.assertIsNotNone(rows[0][1])

    def test_offline_message_is_saved_acked_and_not_duplicated(self):
        self.register("alice", "pass1234")
        bob = self.user("bob", "pass1234")
        bob.send({"type": "message", "to_nick": "alice", "text": "while away", "client_msg_id": "c1"})
        bob.recv(0.5)
        run = self.run_client("alice", duration=3500).finish()
        self.assertEqual(len(run.client_rows("SELECT 1 FROM messages WHERE message = 'while away'")), 1)
        self.assertEqual(self.server.scalar("SELECT delivered FROM messages WHERE message = 'while away'"), 1)

    def test_login_uses_a_single_connection(self):
        self.register("alice", "pass1234")
        before = self.server.log().count('"login"')
        self.run_client("alice", duration=2500).finish()
        self.assertEqual(self.server.log().count('"login"') - before, 1)

    def test_client_reconnects_after_server_restart(self):
        self.register("alice", "pass1234")
        run = self.run_client("alice", duration=9000)
        self.assertIsNotNone(run.wait_for("login-successful"))
        before = self.server.log().count('"login"')
        self.server.restart()
        run.finish()
        self.assertGreater(self.server.log().count('"login"'), before, run.events())
        self.assertEqual(run.end_state()["main_visible"], "1")

    def test_sidebar_lists_everyone_with_online_marks(self):
        self.register("alice", "pass1234")
        self.register("bob", "pass1234")
        self.register("carol", "pass1234")
        self.login("bob", "pass1234")
        run = self.run_client("alice", script="2000:contacts", duration=3500).finish()
        self.assertEqual(run.of("contacts"), ["● bob|○ carol"], run.events())

    def test_presence_updates_the_sidebar_live(self):
        self.register("alice", "pass1234")
        self.register("bob", "pass1234")
        run = self.run_client("alice", script="1500:contacts,4000:contacts,6500:contacts", duration=8000)
        self.wait_contacts(run, 1)
        bob = self.login("bob", "pass1234")
        self.wait_contacts(run, 2)
        bob.close()
        run.finish()
        self.assertEqual(run.of("contacts"), ["○ bob", "● bob", "○ bob"])

    def wait_contacts(self, run, count, timeout=15):
        deadline = time.time() + timeout
        while len(run.of("contacts")) < count and time.time() < deadline:
            time.sleep(0.1)
        self.assertGreaterEqual(len(run.of("contacts")), count, run.events())

    def test_filter_narrows_the_list(self):
        for nick in ("alice", "bob", "bobby", "carol"):
            self.register(nick, "pass1234")
        run = self.run_client("alice", script="1500:contacts", duration=3000).finish()
        self.assertEqual(run.of("contacts"), ["○ bob|○ bobby|○ carol"])

    def test_incoming_message_marks_the_window_title(self):
        self.register("alice", "pass1234")
        bob = self.user("bob", "pass1234")
        run = self.run_client("alice", script="4000:wintitle", duration=5000)
        self.assertIsNotNone(run.wait_for("login-successful"))
        run.wait_for("login-successful")
        time.sleep(1.5)
        bob.send({"type": "message", "to_nick": "alice", "text": "ping", "client_msg_id": "t1"})
        run.finish()
        self.assertEqual(run.of("wintitle"), ["(1) MeZZenger - alice"], run.events())

    def test_client_sends_a_file(self):
        import os, tempfile
        self.register("alice", "pass1234")
        bob = self.user("bob", "pass1234")
        directory = tempfile.mkdtemp(prefix="mezzenger-files-")
        self.addCleanup(__import__("shutil").rmtree, directory, True)
        path = os.path.join(directory, "report.bin")
        data = os.urandom(700_000)
        with open(path, "wb") as f:
            f.write(data)

        run = self.run_client("alice", script=f"1500:search:bob,2500:attach:{path},6000:dump", duration=7000).finish()
        got = bob.recv_type("message", 3)
        self.assertEqual(len(got), 1, run.events())
        self.assertEqual((got[0]["file_name"], got[0]["file_size"]), ("report.bin", len(data)))
        self.assertEqual(support.download_file(self.server, bob, got[0]["file_id"])[1], data)
        rows = run.client_rows("SELECT status, file_name, file_size FROM messages WHERE file_id IS NOT NULL")
        self.assertEqual(rows, [(1, "report.bin", len(data))])
        self.assertIn("report.bin", run.of("view")[-1])

    def test_client_downloads_a_file(self):
        import os, tempfile
        alice = self.user("alice", "pass1234")
        self.register("bob", "pass1234")
        data = os.urandom(400_000)
        ticket, result = support.upload_file(self.server, alice, "pic.bin", data)
        self.assertTrue(result["success"])
        support.send_file_message(alice, "bob", ticket["file_id"], "c1", "вот")
        support.sync(alice)

        target = tempfile.mkdtemp(prefix="mezzenger-dl-")
        self.addCleanup(__import__("shutil").rmtree, target, True)
        run = self.run_client("bob", script=f"1500:search:alice,3000:download:{target},6000:dump", duration=7000).finish()
        saved = os.path.join(target, ticket["file_id"])
        self.assertTrue(os.path.exists(saved), run.events())
        with open(saved, "rb") as f:
            self.assertEqual(f.read(), data)
        self.assertFalse(os.path.exists(saved + ".part"))
        self.assertIn("pic.bin", run.of("view")[-1])
        self.assertEqual(run.client_rows("SELECT file_name, message FROM messages WHERE file_id IS NOT NULL"), [("pic.bin", "вот")])

    def test_client_creates_a_group_and_writes_to_it(self):
        self.register("alice", "pass1234")
        bob = self.user("bob", "pass1234")
        run = self.run_client("alice", script="1500:group:Друзья:bob,3500:send:привет всем,5500:contacts,5600:dump",
                              duration=6500).finish()
        got = bob.recv_type("message", 3)
        self.assertEqual([(m["text"], m["from_nick"]) for m in got], [("привет всем", "alice")], run.events())
        group_id = self.server.scalar("SELECT id FROM chat_groups")
        self.assertEqual(got[0]["group_id"], group_id)
        self.assertEqual(run.client_rows("SELECT message, status, is_outgoing FROM messages WHERE user_id = ?", -group_id),
                         [("привет всем", 1, 1)])
        self.assertIn("👥 Друзья", run.of("contacts")[-1])
        self.assertIn("Вы:", run.of("view")[-1])

    def test_client_receives_a_group_message_under_the_group(self):
        alice = self.user("alice", "pass1234")
        self.register("bob", "pass1234")
        self.register("carol", "pass1234")
        ids = [self.server.scalar("SELECT id FROM users WHERE nickname = ?", n) for n in ("bob", "carol")]
        alice.send({"type": "group_create", "name": "Клуб", "members": ids})
        gid = alice.recv_type("group_result")[0]["group_id"]
        alice.send({"type": "message", "group_id": gid, "text": "собрание в пять", "client_msg_id": "g1"})
        support.sync(alice)

        run = self.run_client("bob", script="2500:contacts,2600:wintitle,3000:search:Клуб,4000:dump", duration=5000).finish()
        self.assertIn("👥 Клуб", run.of("contacts")[0])
        self.assertEqual(run.of("wintitle"), ["(1) MeZZenger - bob"])
        self.assertEqual(run.client_rows("SELECT user_nick, message FROM messages WHERE user_id = ?", -gid),
                         [("alice", "собрание в пять")])
        self.assertIn("alice:", run.of("view")[-1])
        self.assertIn("собрание в пять", run.of("view")[-1])
        self.assertEqual(self.server.scalar("SELECT delivered FROM messages WHERE to_user_id = ?",
                                            self.server.scalar("SELECT id FROM users WHERE nickname = 'bob'")), 1)

    def test_group_disappears_live_when_the_member_is_removed(self):
        alice = self.user("alice", "pass1234")
        self.register("bob", "pass1234")
        self.register("carol", "pass1234")
        ids = [self.server.scalar("SELECT id FROM users WHERE nickname = ?", n) for n in ("bob", "carol")]
        alice.send({"type": "group_create", "name": "Клуб", "members": ids})
        gid = alice.recv_type("group_result")[0]["group_id"]

        run = self.run_client("bob", script="2000:contacts,6000:contacts", duration=7500)
        self.wait_contacts(run, 1)
        alice.send({"type": "group_remove", "group_id": gid, "id": ids[0]})
        alice.recv_type("group_result")
        self.wait_contacts(run, 2)
        run.finish()
        first, second = run.of("contacts")
        self.assertIn("👥 Клуб", first)
        self.assertNotIn("👥", second)

    def test_client_leaves_a_group(self):
        self.register("alice", "pass1234")
        self.register("bob", "pass1234")
        self.register("carol", "pass1234")
        self.run_client("bob", script="1500:group:Клуб:alice;carol,3500:leave:", duration=5000).finish()
        members = [row[0] for row in self.server.sql(
            "SELECT u.nickname FROM group_members gm JOIN users u ON u.id = gm.user_id ORDER BY u.nickname")]
        self.assertEqual(members, ["alice", "carol"])
        self.assertNotEqual(self.server.scalar("SELECT owner_id FROM chat_groups"),
                            self.server.scalar("SELECT id FROM users WHERE nickname = 'bob'"))

    def test_client_shows_a_system_message_when_someone_leaves(self):
        alice = self.user("alice", "pass1234")
        self.register("bob", "pass1234")
        self.register("carol", "pass1234")
        ids = [self.server.scalar("SELECT id FROM users WHERE nickname = ?", n) for n in ("bob", "carol")]
        alice.send({"type": "group_create", "name": "Клуб", "members": ids})
        gid = alice.recv_type("group_result")[0]["group_id"]

        run = self.run_client("bob", script="1500:search:Клуб,5500:dump", duration=7000)
        self.assertIsNotNone(run.wait_for("step", "search"))
        alice.send({"type": "group_remove", "group_id": gid, "id": self.server.scalar("SELECT id FROM users WHERE nickname = 'alice'")})
        alice.recv_type("group_result")
        run.finish()
        self.assertIn("alice покинул(а) группу", run.of("view")[-1])
        self.assertEqual(run.client_rows("SELECT message, is_system FROM messages WHERE user_id = ?", -gid),
                         [("alice покинул(а) группу", 1)])


if __name__ == "__main__":
    unittest.main()
