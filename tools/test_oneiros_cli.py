#!/usr/bin/env python3
"""Host regressions for Oneiros CLI sampling and ONEIROS4 model validation.

Run on Linux x86_64 or WSL. Uses the shipped checkpoint without modifying it;
malformed fixtures and compiled programs live only in a temporary directory.
"""
import os
from pathlib import Path
import platform
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "user/pkg/nyxgen/nyxgen.c"
MODEL = ROOT / "user/pkg/nyxgen/model.bin"

HARNESS = r'''
#define main nyxgen_main
#include "@SOURCE@"
#undef main
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); return 1; } } while (0)
static int sampling_test(void){
    float probabilities[V]={0};
    CHECK(sample_text(probabilities,0)==' ');
    CHECK(sample_text(probabilities,0xFFFFFFFFu)==' ');
    for (int c=0;c<V;c++){
        int expected=(c==9 || c==10 || (c>31 && c<127))?c:' ';
        probabilities[c]=1.0f;
        CHECK(sample_text(probabilities,0)==expected);
        CHECK(sample_text(probabilities,0xFFFFFFFFu)==expected);
        probabilities[c]=0.0f;
    }
    probabilities[0]=1; probabilities[27]=1; probabilities[127]=1; probabilities[255]=1;
    probabilities['A']=0.25f; probabilities['B']=0.75f;
    CHECK(sample_text(probabilities,0)=='A');
    CHECK(sample_text(probabilities,0x3FFFFF00u)=='A');
    CHECK(sample_text(probabilities,0x40000000u)=='B');
    CHECK(sample_text(probabilities,0xFFFFFFFFu)=='B');
    memset(probabilities,0,sizeof(probabilities));
    union { u32 bits; float value; } bad;
    bad.bits=0x7FC00000u; probabilities['A']=bad.value;
    bad.bits=0x7F800000u; probabilities['B']=bad.value;
    probabilities['C']=-0.5f; probabilities['D']=2.0f;
    CHECK(sample_text(probabilities,0xFFFFFFFFu)==' ');
    probabilities['\t']=0.25f; probabilities['\n']=0.75f;
    CHECK(sample_text(probabilities,0)=='\t');
    CHECK(sample_text(probabilities,0x40000000u)=='\n');
    CHECK(sample_text(probabilities,0xFFFFFFFFu)=='\n');
    puts("OK: all 256 tokens, CDF boundaries, zero mass and invalid probabilities");
    return 0;
}
int main(int argc,char **argv){
    if (argc==2 && !strcmp(argv[1],"--sample-test")) return sampling_test();
    return nyxgen_main(argc,argv);
}
'''


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, timeout=30, **kwargs)


def main():
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    # Project NYXCFLAGS, with host libc for execution and strict diagnostics.
    flags = ["-std=gnu99", "-Os", "-ffreestanding", "-m64", "-mno-red-zone",
             "-Wall", "-Wextra", "-Wshadow", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="nyx-oneiros-") as temporary:
        work = Path(temporary)
        harness = work / "oneiros_test.c"
        harness.write_text(HARNESS.replace("@SOURCE@", SOURCE.as_posix()), encoding="utf-8")
        executable = work / "oneiros_test"
        commands = [
            [*cc, *flags, str(harness), "-o", str(executable)],
            [*cc, *flags, "-Wno-comment", "-nostdlib", "-D__TINYC__", "-I", str(ROOT / "user"),
             "-c", str(SOURCE), "-o", str(work / "nyxgen.o")],
        ]
        for command in commands:
            result = run(command)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors="replace"))
        result = run([str(executable), "--sample-test"])
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace"))
        print(result.stdout.decode().strip())

        def cli(model, count="24", temperature="0.7", seed="static void "):
            return run([str(executable), count, temperature, seed],
                       env={**os.environ, "ONEIROS_MODEL": str(model)})

        for temperature in ("0.1", "0.7", "1.5", "1e-40", "3e38"):
            result = cli(MODEL, temperature=temperature)
            assert result.returncode == 0, result.stderr
            assert result.stdout.startswith(b"static void ") and result.stdout.endswith(b"\n")
            assert len(result.stdout) == len(b"static void ") + 24 + 1
            assert all(c in (9, 10) or 32 <= c < 127 for c in result.stdout)
        result = cli(MODEL, count="0", seed="a\x1b[31m\x07Z\t\n")
        assert result.returncode == 0 and result.stdout == b"a [31m Z\t\n\n", result
        for temperature in ("0", "-1", "nan", "inf", "1e99"):
            result = cli(MODEL, temperature=temperature)
            assert result.returncode != 0 and not result.stdout
            assert b"temperature must be positive and finite" in result.stderr
        print("OK: shipped model, text-only output/seed and finite positive temperatures")

        model = MODEL.read_bytes()
        malformed = [b"", model[:7], model[:31], b"BROKEN!!" + model[8:],
                     model[:8] + struct.pack("<i", 255) + model[12:],
                     model[:-1], model + b"unexpected"]
        for offset, bits in ((32, 0x7FC00000), (36, 0x7F800000),
                             (len(model) - 4, 0xFF800000)):
            malformed.append(model[:offset] + struct.pack("<I", bits) + model[offset + 4:])
        for index, content in enumerate(malformed):
            path = work / f"invalid-{index}.bin"
            path.write_bytes(content)
            result = cli(path)
            assert result.returncode != 0 and not result.stdout
            assert b"cannot load model" in result.stderr, result.stderr
        print("OK: malformed magic/dimensions, short/extra payload and NaN/Inf weights")
        print("OK: host and NyxOS libc build paths compile with project flags")


if __name__ == "__main__":
    main()
