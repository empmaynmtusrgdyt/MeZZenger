"""Передача файлов: отдельное соединение по одноразовому токену, права доступа, лимиты, уборка."""
import os
import uuid

import support
from support import (Conn, ServerTestCase, count_of, download_file, request_upload, send_file_message,
                     send_message, sync, upload_file)


def cid():
    return str(uuid.uuid4())


class TransferTests(ServerTestCase):
    def setUp(self):
        super().setUp()
        self.alice = self.user("alice")
        self.register("bob")
        self.register("carol")
        self.data = os.urandom(300_000)

    def files_dir(self):
        return os.path.join(self.server.dir, "files")

    def send_file(self, name="photo.bin", data=None, text="", to="bob"):
        ticket, result = upload_file(self.server, self.alice, name, data if data is not None else self.data)
        self.assertTrue(result and result["success"], result)
        send_file_message(self.alice, to, ticket["file_id"], cid(), text)
        sync(self.alice)
        return ticket["file_id"]

    def test_file_goes_through_and_both_sides_can_download_it(self):
        file_id = self.send_file(text="смотри")
        bob = self.login("bob")
        offered = support.fetch_offline(bob)
        self.assertEqual(len(offered), 1)
        self.assertEqual((offered[0]["file_id"], offered[0]["file_name"], offered[0]["file_size"]),
                         (file_id, "photo.bin", len(self.data)))
        self.assertEqual(offered[0]["message"], "смотри")

        _, received = download_file(self.server, bob, file_id)
        self.assertEqual(received, self.data)
        _, own = download_file(self.server, self.alice, file_id)
        self.assertEqual(own, self.data)

    def test_online_recipient_gets_the_file_fields_live(self):
        bob = self.login("bob")
        file_id = self.send_file(data=b"hello")
        live = bob.recv_type("message")[0]
        self.assertEqual((live["file_id"], live["file_name"], live["file_size"]), (file_id, "photo.bin", 5))

    def test_message_may_have_no_text_when_it_has_a_file(self):
        self.send_file(data=b"x")
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE message = ''"), 1)

    def test_third_user_cannot_download(self):
        file_id = self.send_file()
        carol = self.login("carol")
        ticket, received = download_file(self.server, carol, file_id)
        self.assertFalse(ticket["success"])
        self.assertIsNone(received)

    def test_file_that_was_not_attached_cannot_be_downloaded_by_others(self):
        ticket, result = upload_file(self.server, self.alice, "x.bin", b"abc")
        self.assertTrue(result["success"])
        bob = self.login("bob")
        self.assertFalse(download_file(self.server, bob, ticket["file_id"])[0]["success"])

    def test_names_are_stripped_of_paths(self):
        for sent, expected in (("../../etc/passwd", "passwd"), ("C:\\dir\\a.txt", "a.txt"), ("  ", "file"), ("..", "file")):
            with self.subTest(name=sent):
                ticket = request_upload(self.alice, sent, 10)
                self.assertEqual(ticket["file_name"], expected)

    def test_size_limits(self):
        for size in (0, -5, 50 * 1024 * 1024 + 1):
            with self.subTest(size=size):
                self.assertFalse(request_upload(self.alice, "a", size)["success"])
        self.assertTrue(request_upload(self.alice, "a", 50 * 1024 * 1024)["success"])

    def test_token_works_once_and_only_for_its_direction(self):
        ticket = request_upload(self.alice, "a.bin", 4)
        wrong = Conn(self.server.port)
        self.conns.append(wrong)
        wrong.send({"type": "file_get", "token": ticket["token"]})
        self.assertFalse(wrong.read_frame(3)["success"])

        ticket = request_upload(self.alice, "a.bin", 4)
        first = Conn(self.server.port)
        self.conns.append(first)
        first.send({"type": "file_put", "token": ticket["token"]})
        first.send_raw(b"abcd")
        self.assertTrue(first.read_frame(5)["success"])
        again = Conn(self.server.port)
        self.conns.append(again)
        again.send({"type": "file_put", "token": ticket["token"]})
        self.assertFalse(again.read_frame(3)["success"])

    def test_garbage_token_is_refused(self):
        conn = Conn(self.server.port)
        self.conns.append(conn)
        conn.send({"type": "file_put", "token": "0" * 64})
        self.assertFalse(conn.read_frame(3)["success"])

    def test_interrupted_upload_leaves_nothing_behind(self):
        ticket, _ = upload_file(self.server, self.alice, "big.bin", self.data, close_after=1000)
        sync(self.alice)
        self.server.stop()
        self.server.start()
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM files WHERE id = ?", ticket["file_id"]), 0)
        self.assertEqual(os.listdir(self.files_dir()), [])

    def test_extra_bytes_fail_the_upload(self):
        ticket, result = upload_file(self.server, self.alice, "a.bin", b"abcd", extra=b"zz")
        self.assertFalse(result and result["success"])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM files WHERE id = ?", ticket["file_id"]), 0)

    def test_cannot_attach_a_file_that_belongs_to_someone_else_or_is_unfinished(self):
        bob = self.user("dave")
        ticket, result = upload_file(self.server, bob, "d.bin", b"data")
        self.assertTrue(result["success"])
        send_file_message(self.alice, "bob", ticket["file_id"], cid())
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 0)

        unfinished = request_upload(self.alice, "u.bin", 10)
        send_file_message(self.alice, "bob", unfinished["file_id"], cid())
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 0)

        send_file_message(self.alice, "bob", "nonexistent", cid())
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 0)

    def test_client_cannot_forge_name_or_size(self):
        ticket, result = upload_file(self.server, self.alice, "real.bin", b"1234")
        self.alice.send({"type": "message", "to_nick": "bob", "text": "", "client_msg_id": cid(),
                         "file_id": ticket["file_id"], "file_name": "evil.exe", "file_size": 1})
        sync(self.alice)
        self.assertEqual(self.server.sql("SELECT file_name, file_size FROM messages"), [("real.bin", 4)])

    def test_retry_with_same_client_msg_id_is_deduplicated(self):
        ticket, _ = upload_file(self.server, self.alice, "a.bin", b"abc")
        message_id = cid()
        for _ in range(2):
            send_file_message(self.alice, "bob", ticket["file_id"], message_id)
        self.assertEqual(count_of(sync(self.alice), "message_ack"), 2)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 1)

    def test_too_many_open_tickets(self):
        results = [request_upload(self.alice, "a", 5, ref=str(i))["success"] for i in range(12)]
        self.assertEqual(results.count(True), 10)

    def test_forgotten_files_are_swept_on_restart(self):
        ticket, result = upload_file(self.server, self.alice, "old.bin", b"abc")
        self.server.sql("UPDATE files SET created_at = datetime('now', '-2 days')")
        self.alice.close()
        self.server.restart()
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM files"), 0)
        self.assertEqual(os.listdir(self.files_dir()), [])

    def test_attached_files_survive_the_sweep(self):
        file_id = self.send_file(data=b"keep")
        self.server.sql("UPDATE files SET created_at = datetime('now', '-30 days')")
        self.alice.close()
        self.server.restart()
        alice = self.login("alice")
        self.assertEqual(download_file(self.server, alice, file_id)[1], b"keep")

    def test_tokens_are_not_written_to_the_log(self):
        ticket, _ = upload_file(self.server, self.alice, "a.bin", b"abcd")
        self.assertNotIn(ticket["token"], self.server.log())
