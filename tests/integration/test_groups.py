"""Групповые чаты: создание, участники, рассылка сообщений, права, выход."""
import uuid

import support
from support import (ServerTestCase, count_of, download_file, fetch_offline, send_file_message, send_message,
                     sync, upload_file)


def cid():
    return str(uuid.uuid4())


class GroupTestCase(ServerTestCase):
    def uid(self, nick):
        return self.server.scalar("SELECT id FROM users WHERE nickname = ?", nick)

    def command(self, conn, kind, **fields):
        """Отправляет команду группы и возвращает group_result."""
        conn.send({"type": kind, **fields})
        return conn.recv_type("group_result", 3)[0]

    def make_group(self, owner_conn, name, *nicks):
        result = self.command(owner_conn, "group_create", name=name, members=[self.uid(n) for n in nicks])
        self.assertTrue(result["success"], result)
        return result["group_id"]

    def send_to_group(self, conn, group_id, text, message_id=None, **extra):
        conn.send({"type": "message", "group_id": group_id, "text": text, "client_msg_id": message_id or cid(), **extra})

    def setUp(self):
        super().setUp()
        self.alice = self.user("alice")
        self.register("bob")
        self.register("carol")
        self.register("dave")


class CreateTests(GroupTestCase):
    def test_creator_and_online_members_get_the_group(self):
        bob = self.login("bob")
        gid = self.make_group(self.alice, "друзья", "bob", "carol")
        info = bob.recv_type("group_info")[0]["group"]
        self.assertEqual((info["id"], info["name"], info["owner_id"]), (gid, "друзья", self.uid("alice")))
        self.assertEqual([m["nick"] for m in info["members"]], ["alice", "bob", "carol"])
        self.assertEqual(len(self.alice.recv_type("group_info")), 1)

    def test_offline_member_sees_the_group_after_login(self):
        gid = self.make_group(self.alice, "друзья", "bob")
        conn, _ = support.auth(self.server, "login", "carol", "goodpass", keep=True)
        self.conns.append(conn)
        self.assertEqual(conn.recv_type("group_list")[0]["groups"], [])
        conn, _ = support.auth(self.server, "login", "bob", "goodpass", keep=True)
        self.conns.append(conn)
        groups = conn.recv_type("group_list")[0]["groups"]
        self.assertEqual([(g["id"], g["name"]) for g in groups], [(gid, "друзья")])

    def test_validation(self):
        bob_id = self.uid("bob")
        cases = [("", [bob_id]), ("x" * 41, [bob_id]), ("ok", []), ("ok", [self.uid("alice")]), ("ok", [9999]),
                 ("ok", list(range(1000, 1060))), ("bad\x07name", [bob_id])]
        for name, members in cases:
            with self.subTest(name=name[:10], members=len(members)):
                self.assertFalse(self.command(self.alice, "group_create", name=name, members=members)["success"])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM chat_groups"), 0)

    def test_name_is_trimmed(self):
        self.make_group(self.alice, "  клуб  ", "bob")
        self.assertEqual(self.server.scalar("SELECT name FROM chat_groups"), "клуб")

    def test_group_count_per_user_is_limited(self):
        bob_id = self.uid("bob")
        results = [self.command(self.alice, "group_create", name=f"g{i}", members=[bob_id])["success"] for i in range(52)]
        self.assertEqual(results.count(True), 50)


class MessagingTests(GroupTestCase):
    def setUp(self):
        super().setUp()
        self.bob = self.login("bob")
        self.gid = self.make_group(self.alice, "друзья", "bob", "carol")
        self.bob.recv_type("group_info")

    def test_message_reaches_members_but_not_the_sender_or_outsiders(self):
        dave = self.login("dave")
        self.send_to_group(self.alice, self.gid, "всем привет", "m1")
        acks = sync(self.alice)
        self.assertEqual(count_of(acks, "message_ack"), 1)
        live = self.bob.recv_type("message")[0]
        self.assertEqual((live["text"], live["from_nick"], live["group_id"]), ("всем привет", "alice", self.gid))
        self.assertEqual(count_of(sync(dave), "message"), 0)
        self.assertEqual(count_of(self.alice.recv(0.3), "message"), 0)

    def test_offline_member_gets_it_with_group_id_and_acks_separately(self):
        self.send_to_group(self.alice, self.gid, "для всех", "m1")
        sync(self.alice)
        carol = self.login("carol")
        offered = fetch_offline(carol)
        self.assertEqual([(m["message"], m["group_id"], m["from_nick"]) for m in offered], [("для всех", self.gid, "alice")])
        carol.send({"type": "delivery_ack", "ids": [offered[0]["id"]]})
        sync(carol)
        self.assertEqual(fetch_offline(carol), [])
        # у bob своя строка, его подтверждение carol не затрагивает
        bob_rows = self.server.sql("SELECT delivered FROM messages WHERE to_user_id = ?", self.uid("bob"))
        self.assertEqual(bob_rows, [(0,)])

    def test_retry_is_deduplicated_and_acked_with_the_same_id(self):
        self.send_to_group(self.alice, self.gid, "раз", "same")
        self.send_to_group(self.alice, self.gid, "раз", "same")
        acks = [m for m in sync(self.alice) if m["type"] == "message_ack"]
        self.assertEqual(len(acks), 2)
        self.assertEqual(acks[0]["id"], acks[1]["id"])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 2)

    def test_non_member_cannot_write_and_is_told_to_forget_the_group(self):
        dave = self.login("dave")
        self.send_to_group(dave, self.gid, "я тут")
        got = sync(dave)
        self.assertEqual([m["group_id"] for m in got if m["type"] == "group_removed"], [self.gid])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 0)

    def test_file_in_group_is_available_to_every_member_only(self):
        ticket, result = upload_file(self.server, self.alice, "doc.bin", b"payload")
        self.assertTrue(result["success"])
        self.send_to_group(self.alice, self.gid, "", file_id=ticket["file_id"])
        sync(self.alice)
        live = self.bob.recv_type("message")[0]
        self.assertEqual((live["file_id"], live["file_name"], live["file_size"]), (ticket["file_id"], "doc.bin", 7))
        self.assertEqual(download_file(self.server, self.bob, ticket["file_id"])[1], b"payload")
        carol = self.login("carol")
        self.assertEqual(download_file(self.server, carol, ticket["file_id"])[1], b"payload")
        dave = self.login("dave")
        self.assertFalse(download_file(self.server, dave, ticket["file_id"])[0]["success"])

    def test_direct_messages_still_work_next_to_groups(self):
        send_message(self.alice, "bob", "лично", cid())
        sync(self.alice)
        live = self.bob.recv_type("message")[0]
        self.assertEqual(live["text"], "лично")
        self.assertNotIn("group_id", live)

    def test_message_text_limit_applies(self):
        self.send_to_group(self.alice, self.gid, "x" * 4001)
        got = sync(self.alice)
        self.assertEqual((count_of(got, "error"), count_of(got, "message_ack")), (1, 0))


class MembershipTests(GroupTestCase):
    def setUp(self):
        super().setUp()
        self.bob = self.login("bob")
        self.carol = self.login("carol")
        self.gid = self.make_group(self.alice, "друзья", "bob", "carol")
        for conn in (self.alice, self.bob, self.carol):
            conn.recv_type("group_info")

    def members_of(self, info):
        return [m["nick"] for m in info["group"]["members"]]

    def test_owner_adds_a_member_who_sees_only_later_messages(self):
        self.send_to_group(self.alice, self.gid, "до дейва")
        sync(self.alice)
        dave = self.login("dave")
        result = self.command(self.alice, "group_add", group_id=self.gid, members=[self.uid("dave")])
        self.assertTrue(result["success"], result)
        self.assertEqual(self.members_of(dave.recv_type("group_info")[0]), ["alice", "bob", "carol", "dave"])
        self.assertEqual(self.members_of(self.bob.recv_type("group_info")[0]), ["alice", "bob", "carol", "dave"])
        # из прошлого только служебное сообщение о добавлении, "до дейва" он не видит
        self.assertEqual([m["message"] for m in fetch_offline(dave)], ["alice добавил(а) dave"])
        self.send_to_group(self.alice, self.gid, "после")
        sync(self.alice)
        self.assertEqual([m["text"] for m in dave.recv_type("message")], ["alice добавил(а) dave", "после"])

    def test_only_the_owner_manages_the_group(self):
        dave_id = self.uid("dave")
        self.assertFalse(self.command(self.bob, "group_add", group_id=self.gid, members=[dave_id])["success"])
        self.assertFalse(self.command(self.bob, "group_remove", group_id=self.gid, id=self.uid("carol"))["success"])
        self.assertFalse(self.command(self.bob, "group_rename", group_id=self.gid, name="мой")["success"])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM group_members"), 3)

    def test_add_validation(self):
        self.assertFalse(self.command(self.alice, "group_add", group_id=self.gid, members=[self.uid("bob")])["success"])
        self.assertFalse(self.command(self.alice, "group_add", group_id=self.gid, members=[9999])["success"])
        self.assertFalse(self.command(self.alice, "group_add", group_id=9999, members=[self.uid("dave")])["success"])

    def test_owner_removes_a_member(self):
        result = self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("carol"))
        self.assertTrue(result["success"], result)
        self.assertEqual(self.carol.recv_type("group_removed")[0]["group_id"], self.gid)
        self.assertEqual(self.members_of(self.bob.recv_type("group_info")[0]), ["alice", "bob"])
        self.send_to_group(self.alice, self.gid, "без карол")
        sync(self.alice)
        self.assertEqual(count_of(sync(self.carol), "message"), 0)

    def test_leaving_drops_undelivered_group_messages(self):
        self.send_to_group(self.alice, self.gid, "пропущу")
        sync(self.alice)
        self.carol.close()
        self.command(self.bob, "group_remove", group_id=self.gid, id=self.uid("bob"))
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE to_user_id = ?", self.uid("bob")), 0)
        # у carol осталось само сообщение и служебное "bob покинул(а) группу"
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE to_user_id = ?", self.uid("carol")), 2)

    def test_member_leaves(self):
        result = self.command(self.bob, "group_remove", group_id=self.gid, id=self.uid("bob"))
        self.assertTrue(result["success"])
        self.assertEqual(self.bob.recv_type("group_removed")[0]["group_id"], self.gid)
        self.assertEqual(self.members_of(self.alice.recv_type("group_info")[0]), ["alice", "carol"])

    def test_ownership_moves_when_the_owner_leaves(self):
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("alice"))
        info = self.bob.recv_type("group_info")[0]["group"]
        self.assertEqual(info["owner_id"], self.uid("bob"))
        self.assertTrue(self.command(self.bob, "group_rename", group_id=self.gid, name="новое")["success"])

    def test_group_disappears_when_one_member_is_left(self):
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("bob"))
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("carol"))
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM chat_groups"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM group_members"), 0)
        self.assertEqual(self.alice.recv_type("group_removed")[-1]["group_id"], self.gid)


    def notices(self, conn):
        return [m for m in conn.recv(0.8) if m.get("type") == "message" and m.get("system")]

    def test_leaving_is_announced_to_the_others_only(self):
        self.command(self.bob, "group_remove", group_id=self.gid, id=self.uid("bob"))
        for conn in (self.alice, self.carol):
            got = self.notices(conn)
            self.assertEqual([(m["text"], m["group_id"]) for m in got], [("bob покинул(а) группу", self.gid)])
        self.assertEqual(self.notices(self.bob), [])

    def test_adding_is_announced_to_everyone_including_the_new_members(self):
        dave = self.login("dave")
        self.command(self.alice, "group_add", group_id=self.gid, members=[self.uid("dave")])
        for conn in (self.alice, self.bob, self.carol, dave):
            got = self.notices(conn)
            self.assertEqual([m["text"] for m in got], ["alice добавил(а) dave"], conn)

    def test_adding_several_lists_them_all(self):
        erin = self.user("erin")
        self.login("dave")
        self.command(self.alice, "group_add", group_id=self.gid, members=[self.uid("dave"), self.uid("erin")])
        self.assertEqual([m["text"] for m in self.notices(self.bob)], ["alice добавил(а) dave, erin"])

    def test_nothing_is_announced_when_adding_fails(self):
        self.command(self.bob, "group_add", group_id=self.gid, members=[self.uid("dave")])
        self.command(self.alice, "group_add", group_id=self.gid, members=[self.uid("bob")])
        self.assertEqual(self.notices(self.carol), [])

    def test_removal_is_announced(self):
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("carol"))
        self.assertEqual([m["text"] for m in self.notices(self.bob)], ["carol удалён(а) из группы"])
        self.assertEqual(self.notices(self.carol), [])

    def test_offline_member_gets_the_notice_and_it_does_not_block_other_messages(self):
        self.carol.close()
        self.command(self.bob, "group_remove", group_id=self.gid, id=self.uid("bob"))
        self.send_to_group(self.alice, self.gid, "после ухода")
        sync(self.alice)
        carol = self.login("carol")
        offered = fetch_offline(carol)
        self.assertEqual([(m["message"], bool(m.get("system"))) for m in offered],
                         [("bob покинул(а) группу", True), ("после ухода", False)])
        carol.send({"type": "delivery_ack", "ids": [m["id"] for m in offered]})
        sync(carol)
        self.assertEqual(fetch_offline(carol), [])

    def test_no_notice_when_the_group_is_dissolved(self):
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("bob"))
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("carol"))
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE from_user_id = 0 AND message LIKE '%carol%'"), 0)

    def test_rename_is_announced_to_everyone(self):
        self.assertTrue(self.command(self.alice, "group_rename", group_id=self.gid, name="новое")["success"])
        expected = ["alice переименовал(а) группу: «друзья» → «новое»"]
        for conn in (self.alice, self.bob, self.carol):
            self.assertEqual([m["text"] for m in self.notices(conn)], expected)

    def test_no_announcement_when_the_name_does_not_change_or_rename_fails(self):
        self.command(self.alice, "group_rename", group_id=self.gid, name="друзья")
        self.command(self.bob, "group_rename", group_id=self.gid, name="чужое")
        self.command(self.alice, "group_rename", group_id=self.gid, name="")
        self.assertEqual(self.notices(self.bob), [])

    def test_rename(self):
        self.assertTrue(self.command(self.alice, "group_rename", group_id=self.gid, name=" новое ")["success"])
        self.assertEqual(self.bob.recv_type("group_info")[0]["group"]["name"], "новое")
        self.assertFalse(self.command(self.alice, "group_rename", group_id=self.gid, name="")["success"])

    def test_removed_user_gets_no_group_after_relogin(self):
        self.command(self.alice, "group_remove", group_id=self.gid, id=self.uid("carol"))
        self.carol.close()
        conn, _ = support.auth(self.server, "login", "carol", "goodpass", keep=True)
        self.conns.append(conn)
        self.assertEqual(conn.recv_type("group_list")[0]["groups"], [])


class MigrationTests(ServerTestCase):
    def test_old_unique_index_is_replaced(self):
        names = [row[0] for row in self.server.sql("SELECT name FROM sqlite_master WHERE type = 'index'")]
        self.assertIn("idx_messages_client_msg_to", names)
        self.assertNotIn("idx_messages_client_msg", names)
