"""Доставка сообщений: подтверждения, повторы, порядок, офлайн-очередь, поиск, прочтение."""
import uuid

import support
from support import ServerTestCase, auth, count_of, fetch_offline, send_message, sync


def cid():
    return str(uuid.uuid4())


class LiveDeliveryTests(ServerTestCase):
    def test_message_is_acked_to_sender_and_delivered_with_ids(self):
        alice, bob = self.user("alice"), self.user("bob")
        client_id = cid()
        send_message(alice, "bob", "hello", client_id)

        acks = alice.recv_type("message_ack")
        delivered = bob.recv_type("message")
        self.assertEqual(len(acks), 1)
        self.assertEqual(acks[0]["client_msg_id"], client_id)
        self.assertGreater(acks[0]["id"], 0)
        self.assertEqual(len(delivered), 1)
        self.assertEqual(delivered[0]["id"], acks[0]["id"])
        self.assertEqual(delivered[0]["text"], "hello")
        self.assertEqual(delivered[0]["from_nick"], "alice")
        self.assertEqual(delivered[0]["from_id"], self.server.scalar("SELECT id FROM users WHERE nickname='alice'"))

    def test_sender_does_not_get_its_own_message_back(self):
        alice, _bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "hello", cid())
        self.assertEqual([m["type"] for m in sync(alice)], ["message_ack"])

    def test_new_contact_without_prior_conversation_receives_the_message(self):
        # Регрессия: получатель раньше терял сообщения от тех, кого нет в его списке
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "first contact", cid())
        message = bob.recv_type("message")[0]
        self.assertGreater(message["from_id"], 0)
        self.assertEqual(message["from_nick"], "alice")

    def test_text_survives_unicode_and_special_characters(self):
        alice, bob = self.user("alice"), self.user("bob")
        text = 'Привет 😀 <b>"quoted"</b> \\ \'   line\nbreak'
        send_message(alice, "bob", text, cid())
        self.assertEqual(bob.recv_type("message")[0]["text"], text)


class AcknowledgementTests(ServerTestCase):
    def test_unacked_message_is_offered_again_after_reconnect(self):
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "one", cid())
        live = bob.recv_type("message")[0]
        bob.close()

        bob = self.login("bob")
        offered = fetch_offline(bob)
        self.assertEqual([m["id"] for m in offered], [live["id"]])
        self.assertEqual(offered[0]["message"], "one")
        self.assertEqual(offered[0]["from_nick"], "alice")
        self.assertEqual(offered[0]["from_id"], live["from_id"])

    def test_delivery_ack_stops_redelivery(self):
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "one", cid())
        message_id = bob.recv_type("message")[0]["id"]

        bob.send({"type": "delivery_ack", "ids": [message_id]})
        sync(bob)
        self.assertEqual(fetch_offline(bob), [])
        self.assertEqual(self.server.scalar("SELECT delivered FROM messages WHERE id=?", message_id), 1)

    def test_sender_cannot_confirm_delivery_for_the_recipient(self):
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "one", cid())
        message_id = alice.recv_type("message_ack")[0]["id"]

        alice.send({"type": "delivery_ack", "ids": [message_id]})
        sync(alice)
        self.assertEqual(self.server.scalar("SELECT delivered FROM messages WHERE id=?", message_id), 0)
        self.assertEqual(len(fetch_offline(bob)), 1)

    def test_garbage_in_delivery_ack_is_harmless(self):
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "one", cid())
        bob.recv_type("message")
        for ids in (None, [], ["x", None, -1, 0, 10 ** 12], "7"):
            bob.send({"type": "delivery_ack", "ids": ids})
        sync(bob)
        self.assertEqual(len(fetch_offline(bob)), 1)

    def test_only_acked_messages_are_removed(self):
        alice = self.user("alice")
        self.register("bob")
        for text in ("o0", "o1", "o2"):
            send_message(alice, "bob", text, cid())
        sync(alice)

        bob = self.login("bob")
        offered = fetch_offline(bob)
        self.assertEqual([m["message"] for m in offered], ["o0", "o1", "o2"])
        bob.send({"type": "delivery_ack", "ids": [m["id"] for m in offered[:2]]})
        sync(bob)
        self.assertEqual([m["message"] for m in fetch_offline(bob)], ["o2"])


class RetryTests(ServerTestCase):
    def test_resending_the_same_client_id_is_acked_again_but_not_redelivered(self):
        alice, bob = self.user("alice"), self.user("bob")
        client_id = cid()
        send_message(alice, "bob", "once", client_id)
        first = alice.recv_type("message_ack")[0]
        bob.recv_type("message")

        send_message(alice, "bob", "once", client_id)
        second = alice.recv_type("message_ack")[0]
        self.assertEqual(second["id"], first["id"])
        sync(alice)
        self.assertEqual(count_of(sync(bob), "message"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 1)

    def test_same_client_id_from_different_senders_are_different_messages(self):
        alice, bob = self.user("alice"), self.user("bob")
        shared = cid()
        send_message(alice, "bob", "from alice", shared)
        send_message(bob, "alice", "from bob", shared)
        self.assertEqual(len(alice.recv_type("message_ack")), 1)
        self.assertEqual(len(bob.recv_type("message_ack")), 1)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 2)

    def test_message_without_client_id_is_still_delivered_but_not_acked(self):
        alice, bob = self.user("alice"), self.user("bob")
        alice.send({"type": "message", "to_nick": "bob", "text": "legacy"})
        self.assertEqual(count_of(sync(alice), "message_ack"), 0)
        self.assertEqual(bob.recv_type("message")[0]["text"], "legacy")


class RejectedMessageTests(ServerTestCase):
    def test_self_and_unknown_recipients_get_no_ack_and_are_not_stored(self):
        alice = self.user("alice")
        send_message(alice, "alice", "to myself", cid())
        send_message(alice, "nobody", "to nobody", cid())
        send_message(alice, "", "to empty", cid())
        alice.send({"type": "message", "to_nick": "alice"})
        self.assertEqual(count_of(sync(alice), "message_ack"), 0)
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages"), 0)

    def test_empty_text_is_ignored(self):
        alice, _bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "", cid())
        self.assertEqual(count_of(sync(alice), "message_ack"), 0)


class BacklogTests(ServerTestCase):
    def test_backlog_larger_than_a_batch_arrives_complete_and_in_order(self):
        self.register("alice")
        self.register("bob")
        alice_id = self.server.scalar("SELECT id FROM users WHERE nickname='alice'")
        bob_id = self.server.scalar("SELECT id FROM users WHERE nickname='bob'")
        total = 230
        # Записываем очередь напрямую: через протокол её не отправить быстрее лимита частоты
        self.server.sql_many("INSERT INTO messages (from_user_id, to_user_id, message, delivered, client_msg_id) "
                             "VALUES (?, ?, ?, 0, ?)",
                             [(alice_id, bob_id, f"bulk{i:03d}", f"c{i}") for i in range(total)])

        bob = self.login("bob")
        received = []
        for _ in range(10):
            batch = fetch_offline(bob, 2.0)
            if not batch:
                break
            received += [m["message"] for m in batch]
            bob.send({"type": "delivery_ack", "ids": [m["id"] for m in batch]})
            sync(bob)
        self.assertEqual(received, [f"bulk{i:03d}" for i in range(total)])
        self.assertEqual(fetch_offline(bob), [])
        self.assertEqual(self.server.scalar("SELECT COUNT(*) FROM messages WHERE delivered=0"), 0)

    def test_messages_survive_a_server_restart(self):
        alice = self.user("alice")
        self.register("bob")
        send_message(alice, "bob", "persisted", cid())
        alice.recv_type("message_ack")
        alice.close()
        self.server.restart()

        bob = self.login("bob")
        self.assertEqual([m["message"] for m in fetch_offline(bob)], ["persisted"])


class ContactsTests(ServerTestCase):
    def test_user_list_contains_conversation_partners(self):
        alice = self.user("alice")
        self.register("bob")
        self.register("carol")
        send_message(alice, "bob", "hi", cid())
        alice.recv_type("message_ack")

        bob_conn, _ = auth(self.server, "login", "bob", "goodpass", keep=True)
        self.conns.append(bob_conn)
        users = bob_conn.recv_type("user_list")[0]["users"]
        self.assertEqual([u["nick"] for u in users], ["alice"])

        carol_conn, _ = auth(self.server, "login", "carol", "goodpass", keep=True)
        self.conns.append(carol_conn)
        self.assertEqual(carol_conn.recv_type("user_list")[0]["users"], [])

    def test_search_user(self):
        alice = self.user("alice")
        self.register("bob")
        alice.send({"type": "search_user", "nick": "bob"})
        found = alice.recv_type("search_user_response")[0]
        self.assertTrue(found["success"])
        self.assertEqual(found["user"]["nick"], "bob")
        self.assertGreater(found["user"]["id"], 0)

        alice.send({"type": "search_user", "nick": "nobody"})
        self.assertFalse(alice.recv_type("search_user_response")[0]["success"])
        alice.send({"type": "search_user", "nick": ""})
        self.assertFalse(alice.recv_type("search_user_response")[0]["success"])

    def test_searching_yourself_is_refused(self):
        alice = self.user("alice")
        alice.send({"type": "search_user", "nick": "alice"})
        response = alice.recv_type("search_user_response")[0]
        self.assertFalse(response["success"])
        self.assertIn("самому себе", response["message"])

    def test_mark_messages_read(self):
        alice, bob = self.user("alice"), self.user("bob")
        send_message(alice, "bob", "read me", cid())
        alice.recv_type("message_ack")
        bob.recv_type("message")
        self.assertEqual(self.server.scalar("SELECT read FROM messages"), 0)

        alice_id = self.server.scalar("SELECT id FROM users WHERE nickname='alice'")
        bob.send({"type": "mark_messages_read", "id": alice_id})
        sync(bob)
        self.assertEqual(self.server.scalar("SELECT read FROM messages"), 1)

    def test_one_user_cannot_mark_other_peoples_messages_read(self):
        alice, bob, carol = self.user("alice"), self.user("bob"), self.user("carol")
        send_message(alice, "bob", "private", cid())
        alice.recv_type("message_ack")
        alice_id = self.server.scalar("SELECT id FROM users WHERE nickname='alice'")
        carol.send({"type": "mark_messages_read", "id": alice_id})
        sync(carol)
        self.assertEqual(self.server.scalar("SELECT read FROM messages"), 0)


if __name__ == "__main__":
    import unittest
    unittest.main()
