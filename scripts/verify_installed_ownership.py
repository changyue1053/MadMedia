#!/usr/bin/env python3
"""Linux installed C ABI -> C++ ownership handoff; no transfer work."""

import argparse
import contextlib
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]


class Rejected(RuntimeError):
    pass


def start(argv, *, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL):
    return subprocess.Popen(argv, stdin=stdin, stdout=stdout, stderr=subprocess.DEVNULL,
                            start_new_session=True)


def stop_group(child):
    # The leader has not been reaped, so its process-group ID cannot be reused.
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_group(child, seconds):
    # poll()/wait(timeout=...) could reap the group leader first, making its
    # PID unsafe to use when cleaning up a lingering subprocess.
    fd = os.pidfd_open(child.pid)
    try:
        if not select.select([fd], [], [], max(seconds, 0))[0]:
            raise subprocess.TimeoutExpired(child.args[0], seconds)
    finally:
        os.close(fd)
    stop_group(child)
    return child.wait(timeout=5)


@contextlib.contextmanager
def process(argv, **kwargs):
    child = start(argv, **kwargs)
    try:
        yield child
    finally:
        try:
            if child.returncode is None:
                stop_group(child)
                child.wait(timeout=5)
        finally:
            for stream in (child.stdin, child.stdout):
                if stream is not None:
                    stream.close()


def run(stage, argv, timeout=240):
    print(f"STAGE: {stage}", flush=True)
    try:
        with process(argv) as child:
            if wait_group(child, timeout) != 0:
                raise Rejected(stage)
    except (OSError, subprocess.TimeoutExpired):
        raise Rejected(stage) from None


def get(child, expected, deadline, stage):
    seconds = deadline - time.monotonic()
    if seconds <= 0 or not select.select([child.stdout], [], [], seconds)[0]:
        raise Rejected(stage)
    if os.read(child.stdout.fileno(), 1) != expected:
        raise Rejected(stage)


def put(child, value, stage):
    try:
        if os.write(child.stdin.fileno(), value) != len(value):
            raise Rejected(stage)
    except OSError:
        raise Rejected(stage) from None


def finish(child, deadline, stage):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise Rejected(stage)
    # Require EOF before wait(), and reject extra bytes immediately rather
    # than collecting arbitrary output or letting a full pipe hang wait().
    get(child, b"", deadline, stage)
    try:
        if wait_group(child, deadline - time.monotonic()) != 0:
            raise Rejected(stage)
    except subprocess.TimeoutExpired:
        raise Rejected(stage) from None


def handoff(owner, contender, a, b, limit=20):
    if a == b or a.exists() or b.exists():
        raise Rejected("fresh-directories")
    deadline = time.monotonic() + limit
    with process([owner, a], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as first:
        get(first, b"R", deadline, "c-owner-ready")
        with process([contender, a, b], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as other:
            get(other, b"B", deadline, "contender-busy-and-separate")
            put(first, b"C", "c-owner-destroy-request")
            get(first, b"D", deadline, "c-owner-destroyed-still-live")
            put(other, b"R", "contender-reopen-request")
            get(other, b"P", deadline, "contender-reopened")
            finish(other, deadline, "contender-final")
        put(first, b"Q", "c-owner-finish-request")
        get(first, b"Z", deadline, "c-owner-confirmed-still-live")
        finish(first, deadline, "c-owner-final")


def verify(sha):
    git = ["git", "-C", REPO, "-c", "core.fsmonitor=false"]
    if subprocess.check_output([*git, "rev-parse", "--verify", "HEAD"],
                               stderr=subprocess.DEVNULL, timeout=10).decode("ascii").strip() != sha:
        raise Rejected("exact-checkout")
    if subprocess.check_output([*git, "status", "--porcelain"],
                               stderr=subprocess.DEVNULL, timeout=10):
        raise Rejected("clean-checkout")
    with tempfile.TemporaryDirectory(prefix="madmedia-installed-ownership-") as workspace:
        root = Path(workspace).resolve(strict=True)
        source = root / "disposable-source"
        source.mkdir()
        # Copy exact tracked bytes. No sibling SDK, untracked content or .git.
        with tempfile.TemporaryFile() as archive:
            with process(["git", "-C", REPO, "archive", "--format=tar", sha], stdout=archive) as child:
                if wait_group(child, 30) != 0:
                    raise Rejected("producer-snapshot")
            archive.seek(0)
            with process(["tar", "-xf", "-", "-C", source], stdin=archive) as child:
                if wait_group(child, 30) != 0:
                    raise Rejected("producer-snapshot")
        if (source / ".git").exists():
            raise Rejected("producer-snapshot")
        for role in ("owner", "contender"):
            consumer = root / (role + "-source")
            consumer.mkdir()
            fixture = source / "examples/ownership_handoff"
            for filename in ("CMakeLists.txt", "owner.c" if role == "owner" else "contender.cpp"):
                shutil.copyfile(fixture / filename, consumer / filename)
        build, prefix, installed = (root / name for name in ("producer-build", "prefix", "relocated"))
        run("producer-configure", ["cmake", "-S", source, "-B", build,
                                  "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_LIBDIR=lib",
                                  "-DMADMEDIA_BUILD_TESTS=OFF", "-DMADMEDIA_BUILD_TOOLS=OFF",
                                  "-DMADMEDIA_WITH_MADOBS=OFF", "-DMADMEDIA_BUILD_C_ABI=ON"])
        run("producer-build", ["cmake", "--build", build, "--parallel", "2"])
        run("producer-install", ["cmake", "--install", build, "--prefix", prefix])
        prefix.rename(installed)
        shutil.rmtree(build)
        shutil.rmtree(source)
        if any(p.exists() or p.is_symlink() for p in (build, source, prefix)):
            raise Rejected("producer-removed")
        for role in ("owner", "contender"):
            out = root / (role + "-build")
            run(role + "-configure", ["cmake", "-S", root / (role + "-source"), "-B", out,
                                      "-DCMAKE_BUILD_TYPE=Release",
                                      f"-DINSTALLED={installed}", f"-DROLE={role}"])
            run(role + "-build", ["cmake", "--build", out, "--parallel", "2"])
        handoff(root / "owner-build/ownership", root / "contender-build/ownership",
                root / "app-a", root / "app-b")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sha", required=True)
    args = parser.parse_args()
    if len(args.sha) != 40 or any(c not in "0123456789abcdef" for c in args.sha):
        print("FAIL: sha-format", flush=True)
        return 1
    try:
        verify(args.sha)
    except Rejected as error:
        print(f"FAIL: {error}", flush=True)
        return 1
    except (OSError, subprocess.SubprocessError, UnicodeError):
        print("FAIL: environment-or-process", flush=True)
        return 1
    print("PASS: installed-c-to-cpp-ownership-handoff", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
