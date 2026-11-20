#!/usr/bin/env python3
"""Exercise the bounded pipe protocol with synthetic executables, not the SDK."""

from pathlib import Path
import tempfile
import unittest

from verify_installed_ownership import Rejected, handoff


class ProtocolTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="mmo-test-")
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.a, self.b = self.root / "a", self.root / "b"
        self.owner = self.fixture("owner", """
say('R')
assert get() == 'C'
say('D')
assert get() == 'Q'
say('Z')
""")
        self.contender = self.fixture("contender", """
say('B')
assert get() == 'R'
say('P')
""")

    def fixture(self, name, source):
        path = self.root / name
        path.write_text("#!/usr/bin/env python3\nimport sys\n"
                        "def say(value): print(value, end='', flush=True)\n"
                        "def get(): return sys.stdin.read(1)\n" + source, encoding="ascii")
        path.chmod(0o700)
        return path

    def run_protocol(self, limit=2):
        handoff(self.owner, self.contender, self.a, self.b, limit)

    def test_ordered_success(self):
        self.run_protocol()

    def test_wrong_status(self):
        self.contender = self.fixture("wrong-status", "say('F')")
        with self.assertRaisesRegex(Rejected, r"^contender-busy-and-separate$"):
            self.run_protocol()

    def test_owner_already_gone(self):
        self.owner = self.fixture("gone", "say('R')\nassert get() == 'C'\nsay('D')")
        with self.assertRaisesRegex(Rejected, r"^c-owner-(finish-request|confirmed-still-live)$"):
            self.run_protocol()

    def test_extra_output(self):
        self.contender = self.fixture("extra", "say('B')\nassert get() == 'R'\nsay('PP')")
        with self.assertRaisesRegex(Rejected, r"^contender-final$"):
            self.run_protocol()

    def test_process_deadline(self):
        self.owner = self.fixture("never-ready", "get()")
        with self.assertRaisesRegex(Rejected, r"^c-owner-ready$"):
            self.run_protocol(0.2)

    def test_existing_directory_rejected_before_spawn(self):
        self.a.mkdir()
        with self.assertRaisesRegex(Rejected, r"^fresh-directories$"):
            self.run_protocol()


if __name__ == "__main__":
    unittest.main()
