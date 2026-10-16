#!/usr/bin/env python3
"""Host cat regressions, including controlled short I/O and raw NyxOS errors.

Run on Linux x86_64 or WSL. Fixture files and binaries are temporary.
"""
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define _NYXOS_LIBC_H
static long read_bytes, write_bytes;
static int read_interrupted, write_interrupted, read_failed;
static long setting(const char* name,long fallback){
    const char* value=getenv(name); return value?strtol(value,0,10):fallback;
}
static long test_open(const char* path,int flags,int mode){ return open(path,flags,mode); }
static long test_read(int fd,void* buffer,long size){
    if (!read_interrupted && setting("CAT_TEST_READ_EINTR",0)) { read_interrupted=1; return -4; }
    long fail_after=setting("CAT_TEST_READ_FAIL_AFTER",-1);
    if (!read_failed && fail_after>=0){
        if (read_bytes>=fail_after) { read_failed=1; return -5; }
        if (size>fail_after-read_bytes) size=fail_after-read_bytes;
    }
    long limit=setting("CAT_TEST_READ_LIMIT",size);
    if (size>limit) size=limit;
    long count=read(fd,buffer,(size_t)size);
    if (count<0) return -errno;
    read_bytes+=count; return count;
}
static long test_write(int fd,const void* buffer,long size){
    if (!write_interrupted && setting("CAT_TEST_WRITE_EINTR",0)) { write_interrupted=1; return -4; }
    long fail_after=setting("CAT_TEST_WRITE_FAIL_AFTER",-1);
    long zero_after=setting("CAT_TEST_WRITE_ZERO_AFTER",-1);
    if (fail_after>=0){
        if (write_bytes>=fail_after) return -5;
        if (size>fail_after-write_bytes) size=fail_after-write_bytes;
    }
    if (zero_after>=0){
        if (write_bytes>=zero_after) return 0;
        if (size>zero_after-write_bytes) size=zero_after-write_bytes;
    }
    long limit=setting("CAT_TEST_WRITE_LIMIT",size);
    if (size>limit) size=limit;
    long count=write(fd,buffer,(size_t)size);
    if (count<0) return -errno;
    write_bytes+=count; return count;
}
static long test_close(int fd){
    long result=close(fd);
    return setting("CAT_TEST_CLOSE_ERROR",0)?-5:result;
}
#define open test_open
#define read test_read
#define write test_write
#define close test_close
#include "@ROOT@/user/cat.c"
'''


def numbered(data, nonblank=False):
    output = bytearray()
    start, number = True, 1
    for byte in data:
        if start and (not nonblank or byte != 10):
            output.extend(f"{number:6d}\t".encode())
            number += 1
        output.append(byte)
        start = byte == 10
    return bytes(output)


def main():
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=gnu99", "-Os", "-ffreestanding", "-m64", "-mno-red-zone",
             "-Wall", "-Wextra", "-Wshadow", "-Werror", "-Wno-comment"]
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("CAT_TEST_") and key != "POSIXLY_CORRECT"}
    env["LC_ALL"] = "C"
    with tempfile.TemporaryDirectory(prefix="nyx-cat-") as temporary:
        work = Path(temporary)
        source, executable = work / "cat_test.c", work / "cat_test"
        source.write_text(SHIM.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        for command in (
            [*cc, *flags, str(source), "-o", str(executable)],
            [*cc, *flags, "-nostdlib", "-I", str(ROOT / "user"),
             "-c", str(ROOT / "user/cat.c"), "-o", str(work / "cat.o")],
        ):
            result = subprocess.run(command, capture_output=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors="replace"))

        binary = bytes(range(256)) * 8 + b"tail\0without-newline"
        boundary = b"A" * 511 + b"\n\n" + b"B" * 1023 + b"\n\r\n\t\n\0\n" + b"\n" * 1100
        files = {"binary": binary, "boundary": boundary, "empty": b"",
                 "first": b"first", "second": b" continuation\n\nlast", "-n": b"literal-option\0"}
        for name, content in files.items():
            (work / name).write_bytes(content)

        reference_cat = shutil.which("cat")
        reference_name = ""
        if reference_cat:
            version = subprocess.run([reference_cat, "--version"], capture_output=True, timeout=5)
            if b"coreutils" in version.stdout:
                reference_name = version.stdout.splitlines()[0].decode()
            else:
                reference_cat = None

        checks = 0

        def check(arguments, expected, stdin=b"", faults=None, status=0, diagnostic=b"", compare=True):
            nonlocal checks
            result = subprocess.run([str(executable), *arguments], input=stdin, capture_output=True,
                                    cwd=work, env={**env, **(faults or {})}, timeout=10)
            assert result.returncode == status, (arguments, result.returncode, result.stderr)
            assert result.stdout == expected, (arguments, len(result.stdout), len(expected))
            assert diagnostic in result.stderr if diagnostic else not result.stderr, result.stderr
            if reference_cat and compare and not faults and not status:
                reference = subprocess.run([reference_cat, *arguments], input=stdin, capture_output=True,
                                           cwd=work, env=env, timeout=10)
                assert reference.returncode == 0 and reference.stdout == result.stdout, arguments
            checks += 1
            return result

        check([], binary, stdin=binary)
        check(["binary", "empty", "binary"], binary * 2)
        check(["-", "first", "-", "second"], binary + files["first"] + files["second"], stdin=binary)
        check(["--", "-n"], files["-n"])
        check(["--"], b"", stdin=b"")
        check(["-n"], b"", stdin=b"")
        for options in (["-n"], ["-b"], ["-nb"], ["-bn"], ["-b", "-n"]):
            nonblank = any("b" in option for option in options)
            check([*options, "boundary", "empty", "binary"], numbered(boundary + binary, nonblank))
            combined = files["first"] + b"\n\n" + files["second"]
            check(["first", *options, "-", "second"], numbered(combined, nonblank), stdin=b"\n\n")
        for options in ([], ["-n"], ["-b"]):
            expected = numbered(boundary, options == ["-b"]) if options else boundary
            check(options, expected, stdin=boundary,
                  faults={"CAT_TEST_READ_LIMIT": "7", "CAT_TEST_WRITE_LIMIT": "3",
                          "CAT_TEST_READ_EINTR": "1", "CAT_TEST_WRITE_EINTR": "1"})
        check(["-nb"], numbered(binary, True), stdin=binary,
              faults={"CAT_TEST_READ_LIMIT": "1", "CAT_TEST_WRITE_LIMIT": "1"})
        check(["missing", "binary"], binary, status=1, diagnostic=b"missing: cannot open")
        check(["-z", "binary"], b"", status=1, diagnostic=b"invalid option")
        check(["first", "second"], b"fir" + files["second"],
              faults={"CAT_TEST_READ_FAIL_AFTER": "3"}, status=1, diagnostic=b"first: read error")
        check(["-n", "first", "second"], numbered(b"fir" + files["second"]),
              faults={"CAT_TEST_READ_FAIL_AFTER": "3"}, status=1, diagnostic=b"first: read error")
        check([], b"", faults={"CAT_TEST_READ_FAIL_AFTER": "0"}, status=1, diagnostic=b"-: read error")
        check(["first"], files["first"], faults={"CAT_TEST_CLOSE_ERROR": "1"},
              status=1, diagnostic=b"close error")
        for failure in ("CAT_TEST_WRITE_FAIL_AFTER", "CAT_TEST_WRITE_ZERO_AFTER"):
            for options in ([], ["-n"]):
                expected = numbered(binary) if options else binary
                for position in (0, 3, 9, 515):
                    result = check([*options, "binary", "missing"], expected[:position],
                                   faults={failure: str(position)}, status=1, diagnostic=b"cat: write error\n")
                    assert result.stderr == b"cat: write error\n", result.stderr
        print(f"OK: {checks} cat cases; bytes, numbering, operands, partial I/O and errors")
        print("OK: host and NyxOS project-flags compilation")
        print(f"OK: comparison with {reference_name}" if reference_cat else
              "NOTE: coreutils cat unavailable; exact-byte expectations passed")


if __name__ == "__main__":
    main()
