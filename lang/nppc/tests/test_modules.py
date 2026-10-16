#!/usr/bin/env python3
"""Compile and run the N++ private-module-name regression on Linux x86_64."""

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MODULE = r'''extern syscall {
    fn @WRITE@(fd: i32, buf: *u8, len: isize) -> i64 = 1
}

fn put(s: str) { @WRITE@(1, s.ptr as *u8, s.len as isize); }

struct Record { value: i64 }
struct Box<T> { value: T }
enum Choice<T> { Some(value: T), None }

#[drop(release)]
own struct Guard<T> { value: T }

fn release<T>(g: Guard<T>) { put("drop {g.value}\n"); }

fn helper() -> i64 {
    r := Record{ value: @VALUE@ };
    b := Box<i64>{ value: r.value };
    c := Choice<i64>.Some{ value: b.value };
    return match c { Some(v) => v, None => 0 };
}

pub fn @EXPORT@() -> i64 {
    g := Guard<i64>{ value: helper() };
    g.value
}
'''
MAIN = r'''use "first.npp";
use "second.npp";
use "a-b.npp";
use "a_b.npp";
use "a/util.npp";

extern syscall {
    fn write_main(fd: i32, buf: *u8, len: isize) -> i64 = 1
}

fn main() -> i64 {
    a := value_a();
    b := value_b();
    c := value_c();
    d := value_d();
    msg := "values {a} {b} {c} {d}\n";
    write_main(1, msg.ptr as *u8, msg.len as isize);
    0
}
'''


def run(args):
    result = subprocess.run(
        [str(arg) for arg in args], capture_output=True, text=True, timeout=30
    )
    if result.returncode:
        raise RuntimeError(
            f"{' '.join(str(arg) for arg in args)} failed ({result.returncode})\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nppc", type=Path, help="test an already-built compiler")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        parser.error("the runtime shim requires Linux x86_64; use WSL on Windows")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nppc-modules-") as temporary:
        work = Path(temporary)
        nppc = args.nppc.resolve() if args.nppc else work / "nppc"
        ncc = work / "ncc"
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror"]
        if not args.nppc:
            run([*cc, *flags, ROOT / "nppc/nppc.c", "-o", nppc])
        run([*cc, *flags, ROOT / "ncc/ncc.c", "-o", ncc])

        modules = ("a/util.npp", "b/util.npp", "a-b.npp", "a_b.npp")
        for index, relative in enumerate(modules):
            target = work / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(
                MODULE.replace("@VALUE@", str((index + 1) * 11)).replace(
                    "@EXPORT@", f"value_{chr(ord('a') + index)}"
                ).replace(
                    "@WRITE@", f"write_{chr(ord('a') + index)}"
                ),
                encoding="utf-8",
            )
        # Nested modules are recorded in a different order from their file ids.
        # Both bridges reach a/util, exercising a re-exported diamond as well.
        (work / "first.npp").write_text(
            'pub use "a/util.npp";\npub use "b/util.npp";\n', encoding="utf-8"
        )
        (work / "second.npp").write_text(
            'pub use "a/util.npp";\n', encoding="utf-8"
        )
        source = work / "main.npp"
        source.write_text(MAIN, encoding="utf-8")
        lowered, repeated = work / "main.n", work / "repeated.n"
        run([nppc, source, "-o", lowered])
        run([nppc, source, "-o", repeated])
        if lowered.read_bytes() != repeated.read_bytes():
            raise RuntimeError("module lowering changed across identical invocations")

        generated = work / "main.c"
        run([ncc, lowered, "-o", generated])
        # Quoted includes search the source's own directory before -I paths.
        # Keep the runtime beside the host shim, away from user/nyxrt.h.
        shutil.copyfile(ROOT.parent / "user/nyxrt.c", work / "nyxrt.c")
        shutil.copyfile(ROOT / "ncc/host/nyxrt.h", work / "nyxrt.h")
        binary = work / "program"
        # N uses its own 64-bit entry signature; check C99 with that ABI.
        run([*cc, "-std=c99", "-pedantic-errors", "-ffreestanding", "-O2", generated,
             work / "nyxrt.c", "-o", binary])
        output = run([binary])
        expected = "drop 11\ndrop 22\ndrop 33\ndrop 44\nvalues 11 22 33 44\n"
        if output != expected:
            raise RuntimeError(f"unexpected module output: {output!r}; wanted {expected!r}")
    print("OK: distinct private functions, types and generic drops; nested/diamond imports; deterministic lowering")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
