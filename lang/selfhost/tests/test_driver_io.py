#!/usr/bin/env python3
"""Check all four N selfhost input drivers against real files and ncc on Linux."""

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
LIMIT = 256 * 1024
TOOLS = ("lex", "parse", "check", "gen")
SOURCE = (b"fn identity(x: i64) -> i64 { x }\n"
          b"fn apply(f: fn(i64) -> i64, x: i64) -> i64 { f(x) }\n"
          b"fn main() -> i64 { apply(identity, 7) - 7 }\n")


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], capture_output=True,
                          timeout=90, **kwargs)


def require(args, **kwargs):
    result = run(args, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {args}\n"
                           f"{result.stdout.decode(errors='replace')}"
                           f"{result.stderr.decode(errors='replace')}")
    return result.stdout


def instrument_shim(text):
    """Instrument only temporary builds; production runtime is unchanged.

    Linux reads are still real syscalls. Assert every read fits the source
    allocation and that the input descriptor closes. Optional fault modes
    force short reads, a read error after data, or initial allocation failure.
    """
    anchor = "static inline nyx_i64 __nyx_syscall6"
    state = """#include <stdlib.h>
static nyx_i64 __driver_base, __driver_capacity;
static nyx_i64 __driver_fd = -1;
static unsigned __driver_reads;
static void __attribute__((destructor)) __driver_closed(void) {
    if (__driver_fd >= 0) __builtin_trap();
}

"""
    assert text.count(anchor) == 1
    text = text.replace(anchor, state + anchor)
    anchor = "    if (no == 7) {"
    hook = """    const char* fault = getenv("NYX_DRIVER_TEST_FAULT");
    if (no == 7 && !__driver_base && fault && fault[0] == 'a') return -1;
    if (no == 4) {
        if (!__driver_base || a2 < __driver_base || a3 < 0 ||
            a2 - __driver_base > __driver_capacity ||
            a3 > __driver_capacity - (a2 - __driver_base)) __builtin_trap();
        if (fault && fault[0] == 'r' && __driver_reads++ > 0) return -5;
        if (fault && fault[0] == 's' && a3 > 7) a3 = 7;
    }
"""
    assert text.count(anchor) == 1
    text = text.replace(anchor, hook + anchor)
    anchor = "        __shim_brk += (nyx_u64)a1;"
    assert text.count(anchor) == 1
    text = text.replace(anchor, """        if (!__driver_base) {
            __driver_base = old;
            __driver_capacity = a1;
        }
""" + anchor)
    anchor = "    return ret;"
    assert text.count(anchor) == 1
    return text.replace(anchor, """    if (no == 2 && ret >= 0) __driver_fd = ret;
    if (no == 3 && ret == 0 && a1 == __driver_fd) __driver_fd = -1;
""" + anchor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, default=ROOT / "selfhost",
                        help="directory containing lex/parse/check/gen.n (also tests a baseline)")
    parser.add_argument("--corpus", action="store_true",
                        help="also compare all N examples/selfhost sources and checker refusals")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("the runtime shim requires Linux x86_64; use WSL on Windows")
    # Baseline runs may deliberately trip the temporary shim's bounds check.
    # Keep those child failures from leaving core dumps in the workspace.
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    fallbacks = {tool: Path(f"/tmp/n_{tool}_target.n") for tool in TOOLS}
    for path in fallbacks.values():
        if path.exists() or path.is_symlink():
            raise RuntimeError(f"refusing to replace an existing fallback fixture: {path}")
    cc = shlex.split(os.environ.get("CC", "cc"))
    failures = []
    with tempfile.TemporaryDirectory(prefix="n-driver-io-") as temporary:
        work = Path(temporary)
        ncc = work / "ncc"
        require([*cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                 ROOT / "ncc/ncc.c", "-o", ncc])
        shutil.copyfile(ROOT.parent / "user/nyxrt.c", work / "nyxrt.c")
        (work / "nyxrt.h").write_text(
            instrument_shim((ROOT / "ncc/host/nyxrt.h").read_text()), encoding="utf-8"
        )
        for tool in TOOLS:
            require([ncc, args.sources.resolve() / f"{tool}.n", "-o", work / f"{tool}.c"])
            require([*cc, "-std=c99", "-pedantic-errors", "-ffreestanding", "-O2",
                     work / f"{tool}.c", work / "nyxrt.c", "-o", work / tool])

        clean = work / "source with spaces.n"
        clean.write_bytes(SOURCE)
        empty = work / "empty.n"
        empty.write_bytes(b"")
        for size in (LIMIT - 1, LIMIT, LIMIT + 1, 2 * LIMIT + 1):
            (work / f"size-{size}.n").write_bytes(SOURCE + b" " * (size - len(SOURCE)))
        missing = work / "missing.n"

        def reference(tool, path):
            if tool == "lex":
                return require([ncc, path, "--tokens"])
            if tool == "parse":
                return require([ncc, path, "--ast"])
            result = run([ncc, path, "-o", work / "reference.c"])
            if result.returncode:
                prefix = (str(path) + ":").encode()
                if result.returncode != 1 or not result.stderr.startswith(prefix):
                    raise RuntimeError(f"unexpected ncc refusal: {result.stderr!r}")
                return result.stderr[len(prefix):]
            return b"" if tool == "check" else (work / "reference.c").read_bytes()

        def expect(tool, label, argv, code, output, fault=None, cwd=None):
            env = dict(os.environ)
            env.pop("NYX_DRIVER_TEST_FAULT", None)
            if fault:
                env["NYX_DRIVER_TEST_FAULT"] = fault
            result = run([work / tool, *argv], env=env, cwd=cwd)
            if (result.returncode, result.stdout, result.stderr) != (code, output, b""):
                # A signal from the shim flags an out-of-bounds read request or
                # leaked descriptor, independently of the textual output.
                failures.append(f"n{tool} {label}: wanted rc={code}, output={output[:150]!r}; "
                                f"got rc={result.returncode}, stdout={result.stdout[:150]!r}, "
                                f"stderr={result.stderr[:150]!r}")

        for tool in TOOLS:
            name = f"n{tool}"
            skipped = "check" if tool == "gen" else tool  # historical ngen wording
            expect(tool, "missing argv", [missing], 1, f"{name}: cannot open {missing}\n".encode())
            expect(tool, "missing fallback", [], 0, f"(no {skipped} target - skipped)\n".encode())
            expect(tool, "directory read", [work], 1, f"{name}: cannot read {work}\n".encode())
            expect(tool, "empty source", [empty], 0, b"")  # established empty-file behavior
            expected = reference(tool, clean)
            expect(tool, "valid argv", [clean], 0, expected)
            # ngen's provenance spells the exact argv; use ncc with that same
            # relative spelling instead of normalizing its generated comment.
            if tool == "gen":
                require([ncc, clean.name, "-o", work / "relative.c"], cwd=work)
                relative = (work / "relative.c").read_bytes()
            else:
                relative = expected
            expect(tool, "relative argv", [clean.name], 0, relative, cwd=work)
            expect(tool, "extra argv unchanged", [clean, "unused"], 0, expected)
            expect(tool, "short reads", [clean], 0, expected, fault="short")
            expect(tool, "read error after data", [clean], 1,
                   f"{name}: cannot read {clean}\n".encode(), fault="read")
            expect(tool, "input allocation failure", [clean], 1,
                   f"{name}: cannot allocate input buffer\n".encode(), fault="alloc")
            for size in (LIMIT - 1, LIMIT):
                path = work / f"size-{size}.n"
                expect(tool, f"{size} bytes", [path], 0, reference(tool, path))
            for size in (LIMIT + 1, 2 * LIMIT + 1):
                path = work / f"size-{size}.n"
                expect(tool, f"{size} bytes refused", [path], 1,
                       f"{name}: input exceeds {LIMIT} bytes: {path}\n".encode())

            fallback = fallbacks[tool]
            owned = None
            try:
                with fallback.open("xb") as handle:
                    owned = os.fstat(handle.fileno())
                    handle.write(SOURCE)
                expect(tool, "present fallback", [], 0, reference(tool, fallback))
                expect(tool, "missing argv never falls back", [missing], 1,
                       f"{name}: cannot open {missing}\n".encode())
                fallback.write_bytes(SOURCE + b" " * (LIMIT + 1 - len(SOURCE)))
                expect(tool, "oversized fallback", [], 1,
                       f"{name}: input exceeds {LIMIT} bytes: {fallback}\n".encode())
            finally:
                if owned is not None:
                    current = fallback.lstat()
                    if (current.st_dev, current.st_ino) != (owned.st_dev, owned.st_ino):
                        raise RuntimeError(f"fallback changed ownership; left it untouched: {fallback}")
                    fallback.unlink()

        if args.corpus:
            sources = sorted((ROOT / "examples").glob("*.n")) + sorted(args.sources.resolve().glob("*.n"))
            for source in sources:
                for tool in TOOLS:
                    expect(tool, str(source), [source], 0, reference(tool, source))
            bad = sorted((ROOT / "selfhost/tests/bad").glob("*.n"))
            for source in bad:
                for tool in ("check", "gen"):
                    expect(tool, source.name, [source], 1, reference(tool, source))
            print(f"Compared {len(sources)} valid sources and {len(bad)} refusals with ncc")
    if failures:
        raise RuntimeError("\n".join(failures))
    print("OK: four drivers; explicit/fallback paths; read/allocation failures; short reads; "
          "256 KiB boundary; bounded read buffers and closed descriptors")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
