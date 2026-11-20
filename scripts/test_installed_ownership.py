#!/usr/bin/env python3
"""Exercise the bounded pipe protocol with synthetic executables, not the SDK."""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

from verify_installed_ownership import MAX_DIAGNOSTIC_BYTES, Rejected, classify_failure, handoff, run


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


class FailureLabelTest(unittest.TestCase):
    def test_configure_failures_are_separate(self):
        inputs = {
            b"  No CMAKE_C_COMPILER could be found.": "c-compiler",
            b"-- The CXX compiler identification is unknown": "cxx-compiler",
            b"  Could NOT find Threads (missing: Threads_FOUND)": "system-threads",
            b"  Could NOT find SQLite3 (missing: SQLite3_LIBRARY SQLite3_INCLUDE_DIR)": "system-sqlite-development",
            b"  Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)": "transitive-curl-development",
        }
        for message, label in inputs.items():
            with self.subTest(label=label):
                log = b"CMake Error in the temporary job path:\r\n" + message + b"\r\n"
                self.assertEqual(classify_failure("producer-configure", log), "producer-configure:" + label)

    def test_no_success_or_unrelated_message_is_promoted(self):
        for log in (b"  Found CURL (8.5.0)", b"  unrelated error from CURL:", b"  unknown label: other"):
            self.assertEqual(classify_failure("producer-configure", log), "producer-configure")
        self.assertEqual(classify_failure("producer-configure", b"x" * (MAX_DIAGNOSTIC_BYTES + 1)),
                         "producer-configure:output-limit")
        both = b"  Could NOT find Threads (missing: Threads_FOUND)\n  No CMAKE_C_COMPILER could be found."
        self.assertEqual(classify_failure("producer-configure", both), "producer-configure")

    def test_link_diagnostic_is_not_configure_diagnostic(self):
        log = b"/usr/bin/ld: /test-only/preview.a(x.o): undefined reference to `curl_multi_init'\n"
        self.assertEqual(classify_failure("producer-configure", log), "producer-configure")
        self.assertEqual(classify_failure("owner-build", log), "owner-build:transitive-curl-link")
        self.assertEqual(classify_failure("contender-build", b"ld: cannot find -lcurl: file not found"),
                         "contender-build:transitive-curl-link")
        self.assertEqual(classify_failure("contender-build", b"ld: cannot find -lcurl-other"),
                         "contender-build")

    def test_nonzero_subprocess_does_not_expose_stdout_or_stderr(self):
        out, err = io.StringIO(), io.StringIO()
        script = ("import sys; print('private-out'); "
                  "print('  Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)', file=sys.stderr); "
                  "print('private-err', file=sys.stderr); raise SystemExit(1)")
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            with self.assertRaisesRegex(Rejected, r"^producer-configure:transitive-curl-development$"):
                run("producer-configure", [sys.executable, "-c", script], timeout=2)
        self.assertEqual(out.getvalue(), "STAGE: producer-configure\n")
        self.assertEqual(err.getvalue(), "")


    def test_oversized_success_keeps_success_and_does_not_print_output(self):
        script = "import os; os.write(1, b'x' * 262144); os.write(2, b'y' * 262144)"
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            run("producer-configure", [sys.executable, "-c", script], timeout=3)
        self.assertEqual(out.getvalue(), "STAGE: producer-configure\n")
        self.assertEqual(err.getvalue(), "")

    def test_oversized_failure_preserves_failure_and_does_not_print_output(self):
        script = ("import os; os.write(2, b'  Could NOT find CURL (missing: CURL_LIBRARY)\\n'); "
                  "os.write(1, b'x' * 262144); raise SystemExit(42)")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            with self.assertRaisesRegex(Rejected, r"^producer-configure:output-limit$"):
                run("producer-configure", [sys.executable, "-c", script], timeout=3)
        self.assertEqual(out.getvalue(), "STAGE: producer-configure\n")
        self.assertEqual(err.getvalue(), "")

    def test_continuously_writable_pipe_does_not_extend_deadline(self):
        script = "import os\nwhile True: os.write(1, b'x' * 8192)"
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(Rejected, r"^producer-configure:timeout$"):
                run("producer-configure", [sys.executable, "-c", script], timeout=0.25)


    def test_exited_leader_wins_over_descendant_holding_pipe(self):
        for status in (0, 42):
            with self.subTest(status=status), contextlib.redirect_stdout(io.StringIO()):
                # Leader exits while its owned descendant still holds stdout
                # and stderr. A pipe-only runner would wait for the deadline.
                script = (
                    "import os, signal\n"
                    "if os.fork() == 0:\n"
                    "    signal.pause()\n"
                    "    os._exit(127)\n"
                    "os.write(2, b'  Could NOT find CURL (missing: CURL_LIBRARY)\\n')\n"
                    f"os._exit({status})\n"
                )
                argv = [sys.executable, "-c", script]
                if status == 0:
                    run("producer-configure", argv, timeout=3)
                else:
                    with self.assertRaisesRegex(Rejected, r"^producer-configure:transitive-curl-development$"):
                        run("producer-configure", argv, timeout=3)


if __name__ == "__main__":
    unittest.main()
