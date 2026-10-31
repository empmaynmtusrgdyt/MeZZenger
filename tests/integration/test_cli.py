"""Команда выпуска кодов приглашения: mezzenger_server --new-invite."""
import os
import re
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

import support
from support import Server, ServerTestCase, auth

CODE_FORMAT = re.compile(r"^[A-HJKMNP-TV-Z2-9]{4}(-[A-HJKMNP-TV-Z2-9]{4}){3}$")


class NewInviteTests(unittest.TestCase):
    def test_prints_only_a_well_formed_code_and_creates_the_database(self):
        directory = tempfile.mkdtemp(prefix="mezzenger-invite-")
        self.addCleanup(shutil.rmtree, directory, True)
        result = subprocess.run([support.SERVER, "--new-invite"], cwd=directory, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(len(lines), 1, f"в stdout должен быть только код: {result.stdout!r}")
        self.assertRegex(lines[0], CODE_FORMAT)
        self.assertTrue(os.path.exists(os.path.join(directory, "server.db")))

    def test_only_a_hash_of_the_code_is_stored(self):
        server = Server()
        self.addCleanup(server.cleanup)
        code = server.invite()
        stored = [row[0] for row in server.sql("SELECT code_hash FROM invites")]
        self.assertEqual(len(stored), 1)
        self.assertEqual(len(stored[0]), 64)
        self.assertNotIn(code.replace("-", ""), stored[0].upper())
        self.assertNotEqual(stored[0], code)

    def test_codes_are_different_every_time(self):
        server = Server()
        self.addCleanup(server.cleanup)
        codes = {server.invite() for _ in range(20)}
        self.assertEqual(len(codes), 20)


class WhileServerRunsTests(ServerTestCase):
    def test_codes_created_while_the_server_runs_are_usable(self):
        first, second = self.server.invite(), self.server.invite()
        self.assertNotEqual(first, second)
        self.assertTrue(auth(self.server, "register", "inv1", "goodpass", first)["success"])
        self.assertTrue(auth(self.server, "register", "inv2", "goodpass", second)["success"])


if __name__ == "__main__":
    unittest.main()
