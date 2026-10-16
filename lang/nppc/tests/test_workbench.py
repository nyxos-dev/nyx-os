#!/usr/bin/env python3
"""Run the bounded N++ workbench and its ownership fences on Linux x86_64."""

import argparse
import difflib
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
EXPECTED = """N++ workbench
mapped 3 4, total 9, ready
request finished
release 10
2 + 3: five
request finished
release 11
2 + x: error 120
request finished
release 12
1 + 1: error 2
emit 90
release 90
close 90
published five
preview 9 on 91
close 91
preview result 92
cancelled pending publish
release 92
close 92
"""
ATTEMPT = """
fn attempt(fail: bool) -> Result<i64, i64> {
    if fail { return Result.Err{ e: 23 }; }
    Result.Ok{ v: 7 }
}
"""


def run(args, **kwargs):
    kwargs.setdefault("text", True)
    return subprocess.run([str(arg) for arg in args], capture_output=True,
                          timeout=90, **kwargs)


def require(args, **kwargs):
    result = run(args, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{' '.join(str(arg) for arg in args)} failed "
                           f"({result.returncode})\n{result.stdout}{result.stderr}")
    return result.stdout


def equal(actual, expected, label):
    if actual != expected:
        if isinstance(actual, bytes):
            actual = actual.decode("utf-8", errors="replace")
            expected = expected.decode("utf-8", errors="replace")
        diff = "".join(difflib.unified_diff(expected.splitlines(True),
                                          actual.splitlines(True),
                                          fromfile="expected", tofile="actual"))
        raise RuntimeError(f"{label} differs:\n{diff}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ncc", type=Path, help="test an already-built ncc (baseline regression)")
    parser.add_argument("--examples", action="store_true",
                        help="also compare ncc/ngen C and runtime on the 25 earlier N++ programs")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("the runtime shim requires Linux x86_64; use WSL on Windows")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nppc-workbench-") as temporary:
        work = Path(temporary)
        ncc = args.ncc.resolve() if args.ncc else work / "ncc"
        nppc, ngen, ncheck = (work / name for name in ("nppc", "ngen", "ncheck"))
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror"]
        if not args.ncc:
            require([*cc, *flags, ROOT / "ncc/ncc.c", "-o", ncc])
        require([*cc, *flags, ROOT / "nppc/nppc.c", "-o", nppc])
        shutil.copyfile(ROOT.parent / "user/nyxrt.c", work / "nyxrt.c")
        shutil.copyfile(ROOT / "ncc/host/nyxrt.h", work / "nyxrt.h")

        def build(source, binary):
            require([*cc, "-std=c99", "-pedantic-errors", "-ffreestanding", "-O2",
                     source, work / "nyxrt.c", "-o", binary])

        def lower(source, name):
            target = work / f"{name}.n"
            require([nppc, source, "-o", target])
            return target

        def emit(source, name):
            target = work / f"{name}.c"
            require([ncc, source, "-o", target])
            return target

        # This check runs first so an old ncc demonstrates the missing Err-path
        # destructor without needing to bootstrap the rest of the toolbox.
        lowered = lower(ROOT / "examples/workbench.npp", "workbench")
        repeated = lower(ROOT / "examples/workbench.npp", "repeated")
        equal(repeated.read_bytes(), lowered.read_bytes(), "deterministic lowering")
        generated = emit(lowered, "workbench")
        build(generated, work / "workbench")
        equal(require([work / "workbench"]), EXPECTED, "workbench cleanup trace")

        for source, binary in (("gen", ngen), ("check", ncheck)):
            build(emit(ROOT / f"selfhost/{source}.n", source), binary)

        def differential(source, name, expected=None):
            output = emit(source, name)
            selfhost = require([ngen, source], text=False)
            equal(selfhost, output.read_bytes(), f"{name}: ncc/ngen C")
            equal(require([ncheck, source]), "", f"{name}: ncheck acceptance")
            alternate = work / f"{name}-ngen.c"
            alternate.write_bytes(selfhost)
            build(output, work / name)
            build(alternate, work / f"{name}-ngen")
            actual = require([work / name])
            other = require([work / f"{name}-ngen"])
            # hello is the only corpus program whose output contains a pid.
            if name == "hello":
                import re
                actual = re.sub(r"pid=\d+", "pid=PID", actual)
                other = re.sub(r"pid=\d+", "pid=PID", other)
            equal(other, actual, f"{name}: ncc/ngen execution")
            if expected is not None:
                equal(actual, expected, f"{name}: runtime trace")

        differential(lowered, "workbench", EXPECTED)
        api = (ROOT / "examples/workbench/api.npp").as_posix()

        def fixture(name, body):
            source = work / f"{name}.npp"
            source.write_text(f'use "{api}";\n' + body, encoding="utf-8")
            return source

        def driver():
            return f"""
fn observe(fail: bool, serial: i64) {{
    result := checked(fail, serial);
    code := match result {{ Ok(value) => 0, Err(error) => error }};
    put("code {{code}}\\n");
}}
fn main() -> i64 {{ observe(false, 1); observe(true, 2); 0 }}
"""

        # Every accepted ? statement form, both equal and rewrapped Results.
        # Live values survive Ok; Err drops in reverse birth order after defers.
        for form, statement in (("let", "value := attempt(fail)?;"),
                                ("assign", "mut value := 0; value = attempt(fail)?;"),
                                ("expr", "attempt(fail)?;")):
            for result_type, payload in (("i64", "7"), ("str", '"ok"')):
                name = f"try-{form}-{result_type}"
                body = ATTEMPT + f"""
fn checked(fail: bool, serial: i64) -> Result<{result_type}, i64> {{
    first := Guard<Lease>{{ value: Lease{{ serial: serial }}, tag: serial }};
    second := Guard<i64>{{ value: 0, tag: serial + 100 }};
    defer put("defer first\\n");
    defer put("defer second\\n");
    {statement}
    put("live {{first.tag}}\\n");
    Result.Ok{{ v: {payload} }}
}}
""" + driver()
                expected = ("live 1\ndefer second\ndefer first\nrelease 101\nrelease 1\nclose 1\ncode 0\n"
                            "defer second\ndefer first\nrelease 102\nrelease 2\nclose 2\ncode 23\n")
                differential(lower(fixture(name, body), name), name, expected)

        # Held parameters are sinks: only their own fields drop at return.
        # Moving a local into the ? operand must not drop it a second time.
        body = ATTEMPT + """
fn held(container: Guard<Lease>, fail: bool) -> Result<i64, i64> {
    attempt(fail)?;
    Result.Ok{ v: 7 }
}
fn checked(fail: bool, serial: i64) -> Result<i64, i64> {
    moved := Guard<Lease>{ value: Lease{ serial: serial }, tag: serial };
    live := Guard<i64>{ value: 0, tag: serial + 100 };
    value := held(moved, fail)?;
    put("live {live.tag}\\n");
    Result.Ok{ v: value }
}
""" + driver()
        differential(lower(fixture("try-held-moved", body), "try-held-moved"),
                     "try-held-moved", "close 1\nlive 101\nrelease 101\ncode 0\n"
                     "close 2\nrelease 102\ncode 23\n")

        # An assignment target is evaluated only after Ok. Its ownership moves
        # must not suppress Err cleanup, even though its types are checked first.
        body = ATTEMPT + """
extern syscall { fn allocate(incr: i64) -> i64 = 7 }
fn target(container: Guard<Lease>) -> *i64 {
    put("target\\n");
    slot := allocate(8) as *i64;
    slot[0] = 0;
    slot
}
fn checked(fail: bool, serial: i64) -> Result<i64, i64> {
    holder := Guard<Lease>{ value: Lease{ serial: serial }, tag: serial };
    target(holder)[0] = attempt(fail)?;
    Result.Ok{ v: 7 }
}
""" + driver()
        differential(lower(fixture("try-lhs-move", body), "try-lhs-move"),
                     "try-lhs-move", "target\nclose 1\ncode 0\nrelease 2\nclose 2\ncode 23\n")

        # A newly bound payload can shadow an outer owner. Err cleanup must
        # still name the outer value, before the inner binding comes into scope.
        body = ATTEMPT + """
fn checked(fail: bool, serial: i64) -> Result<i64, i64> {
    pending := Guard<Lease>{ value: Lease{ serial: serial }, tag: serial };
    if true {
        pending := attempt(fail)?;
        put("value {pending}\\n");
    }
    Result.Ok{ v: 7 }
}
""" + driver()
        differential(lower(fixture("try-shadow", body), "try-shadow"),
                     "try-shadow", "value 7\nrelease 1\nclose 1\ncode 0\n"
                     "release 2\nclose 2\ncode 23\n")

        def rejected(name, body, message, frontend=False):
            source = fixture(name, body)
            if frontend:
                result = run([nppc, source, "-o", work / "bad.n"])
                if result.returncode != 1 or message not in result.stderr:
                    raise RuntimeError(f"{name}: missing nppc refusal: {result}")
                return
            source = lower(source, name)
            reference = run([ncc, source, "-o", work / "bad.c"])
            if reference.returncode != 1 or message not in reference.stderr:
                raise RuntimeError(f"{name}: missing ncc refusal: {reference}")
            prefix = f"{source}:"
            if not reference.stderr.startswith(prefix):
                raise RuntimeError(f"{name}: unexpected diagnostic: {reference.stderr}")
            diagnostic = reference.stderr[len(prefix):]
            for compiler in (ngen, ncheck):
                actual = run([compiler, source])
                if actual.returncode != 1 or actual.stderr:
                    raise RuntimeError(f"{name}: missing {compiler.name} refusal: {actual}")
                equal(actual.stdout, diagnostic, f"{name}: {compiler.name} diagnostic")

        rejected("private", "fn main() -> i64 { helper(50) }\n", "is private to", True)
        rejected("own-fn", """
fn main() -> i64 {
    lease := Lease{ serial: 1 };
    apply<i64>(fn(x: i64) -> i64 { x + lease.serial }, 1)
}
""", "lambda captures own value", True)
        rejected("twice-once", """
fn main() -> i64 {
    publish := emit_once<i64>(1, 2);
    first := publish(0);
    publish(first)
}
""", "after move")

        # No destructor: consuming only after ? cannot discharge its Err path.
        for form, statement in (("let", "value := attempt(fail)?;"),
                                ("assign", "mut value := 0; value = attempt(fail)?;"),
                                ("expr", "attempt(fail)?;")):
            rejected(f"try-leak-{form}", ATTEMPT + """
own struct Manual { serial: i64 }
fn consume_manual(value: Manual) {}
fn checked(fail: bool) -> Result<i64, i64> {
    pending := Manual{ serial: 1 };
""" + statement + """
    consume_manual(pending);
    Result.Ok{ v: 0 }
}
fn main() -> i64 { 0 }
""", "unconsumed own value 'pending' at this return")
        rejected("try-held-leak", ATTEMPT + """
own struct Manual { serial: i64 }
own struct Envelope { field: Manual }
fn consume_envelope(value: Envelope) { consume_envelope(value); }
fn checked(pending: Envelope, fail: bool) -> Result<i64, i64> {
    attempt(fail)?;
    consume_envelope(pending);
    Result.Ok{ v: 0 }
}
fn main() -> i64 { 0 }
""", "held own value 'pending' ends here with field 'field' unconsumed")
        rejected("try-lhs-leak", ATTEMPT + """
own struct Manual { serial: i64 }
fn target(value: Manual) -> *i64 { 0 as *i64 }
fn checked(fail: bool) -> Result<i64, i64> {
    pending := Manual{ serial: 1 };
    target(pending)[0] = attempt(fail)?;
    Result.Ok{ v: 0 }
}
fn main() -> i64 { 0 }
""", "unconsumed own value 'pending' at this return")

        if args.examples:
            examples = sorted(path for path in (ROOT / "examples").glob("*.npp")
                              if path.name not in ("modlib.npp", "modutil.npp", "workbench.npp"))
            for source in examples:
                differential(lower(source, source.stem), source.stem)
            print(f"OK: {len(examples)} earlier N++ programs agree through ncc/ngen and execute equally")
    print("OK: workbench trace; ncc/ngen C and execution; ncheck; ? cleanup matrix and ownership/privacy fences")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
