#!/usr/bin/env python3
"""Build the N checker and verify argv targets and its legacy fallback on Linux.

Uses the real ncc compiler and the existing Linux x86_64 runtime shim. The test
temporarily owns /tmp/n_check_target.n; it refuses to overwrite an existing file.
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FALLBACK = Path("/tmp/n_check_target.n")


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], capture_output=True, text=True,
                          timeout=60, **kwargs)


def require_build(args):
    result = run(args)
    if result.returncode:
        raise RuntimeError(f"build failed: {result.stdout}{result.stderr}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "selfhost/check.n",
                        help="checker source to test (also useful for a baseline comparison)")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("the runtime shim requires Linux x86_64; use WSL on Windows")
    if FALLBACK.exists() or FALLBACK.is_symlink():
        raise RuntimeError(f"refusing to replace the existing legacy fixture {FALLBACK}")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="ncheck-cli-") as temporary:
        work = Path(temporary)
        ncc, checker = work / "ncc", work / "ncheck"
        require_build([*cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                       ROOT / "ncc/ncc.c", "-o", ncc])
        require_build([ncc, args.source.resolve(), "-o", work / "check.c"])
        # Quoted includes must find the host shim beside the runtime source.
        shutil.copyfile(ROOT / "ncc/host/nyxrt.h", work / "nyxrt.h")
        shutil.copyfile(ROOT.parent / "user/nyxrt.c", work / "nyxrt.c")
        require_build([*cc, "-std=c99", "-pedantic-errors", "-O2", "-ffreestanding",
                       work / "check.c", work / "nyxrt.c", "-o", checker])

        def compare(source, argv):
            reference = run([ncc, source, "-o", work / "target.c"])
            expected = ""
            if reference.returncode:
                prefix = f"{source}:"
                if not reference.stderr.startswith(prefix):
                    raise RuntimeError(f"unexpected ncc diagnostic: {reference.stderr!r}")
                expected = reference.stderr[len(prefix):]
            actual = run([checker, *argv], cwd=work)
            if actual.returncode != reference.returncode or actual.stdout != expected or actual.stderr:
                raise RuntimeError(
                    f"checker {argv}: wanted rc={reference.returncode}, diagnostic={expected!r}; "
                    f"got rc={actual.returncode}, stdout={actual.stdout!r}, stderr={actual.stderr!r}"
                )

        clean = work / "clean target.n"
        clean.write_text("fn main() -> i64 { 0 }\n", encoding="utf-8")
        bad = work / "bad target.n"
        bad.write_bytes((ROOT / "selfhost/tests/bad/bad_arity.n").read_bytes())
        # A different, invalid fallback makes it observable that argv wins. Use
        # exclusive creation, and remove only this test's inode when finished.
        owned = None
        try:
            with FALLBACK.open("x", encoding="utf-8") as handle:
                handle.write(bad.read_text(encoding="utf-8"))
                owned = os.fstat(handle.fileno())
            compare(clean, [clean])
            compare(clean, [clean.name])       # relative path with spaces
            compare(bad, [bad])
            compare(FALLBACK, [])              # unchanged no-argument fallback
            FALLBACK.write_text(clean.read_text(encoding="utf-8"), encoding="utf-8")
            compare(FALLBACK, [])
            for name in ("hello.n", "args.n"):
                source = ROOT / "examples" / name
                compare(source, [source])
            for name in ("bad_caps.n", "bad_drop_unknown.n", "bad_assign_ty.n"):
                source = ROOT / "selfhost/tests/bad" / name
                compare(source, [source])
        finally:
            if owned is not None:
                current = FALLBACK.lstat()
                if (current.st_dev, current.st_ino) != (owned.st_dev, owned.st_ino):
                    raise RuntimeError("legacy fixture changed ownership during the test; left it untouched")
                FALLBACK.unlink()
        skipped = run([checker], cwd=work)
        if skipped.returncode or skipped.stdout != "(no check target - skipped)\n" or skipped.stderr:
            raise RuntimeError(f"missing-fallback behavior changed: {skipped}")
    print("OK: N checker argv paths, diagnostics against ncc, and legacy fallback")


if __name__ == "__main__":
    main()
