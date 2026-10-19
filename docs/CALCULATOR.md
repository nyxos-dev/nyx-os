# Desktop calculator

Open **Calculator** from Hemera's application menu. The desktop calculator uses
signed 64-bit integers and applies operations from left to right as they are
entered. It supports digits, `+`, `-`, `*`, `/`, `=`/Enter, `C`, and Backspace
while entering a number. Division truncates toward zero.

## Arithmetic and errors

From development version 6.5.217, digit entry and addition, subtraction and
multiplication check for signed overflow before accepting their result. Division
rejects zero and `INT64_MIN / -1`. Values remain within
`-9223372036854775808` through `9223372036854775807`.

An arithmetic error shows `ERR` and clears the pending operation. Operators,
equals and Backspace leave that message visible. `C` resets the calculator to
zero; entering a digit starts a new calculation. For example, `8 / 0 =` shows
`ERR`, then `7 + 2 =` produces `9`. Previously the normal display refresh
overwrote the division-by-zero error with `0`.

The implementation is in `kernel/gui/apps/calc_win.{c,h}`. It uses compiler
overflow intrinsics, a separate error flag and no new dynamic allocation.
The terminal command `calc` uses the separate expression evaluator in
`kernel/core/calc.c`, whose documented arithmetic includes two's-complement
wraparound; this change applies to the desktop application.

## Verification

Run the focused host regression on Linux or WSL:

```sh
python3 tools/test_calculator.py --compare-ref ed04658
```

The actual desktop implementation runs under ASan/UBSan. An independent
128-bit arithmetic oracle covers 5,576 boundary/random operand pairs, alongside
keyboard sequences, sticky error state, recovery, Backspace and allocation
failure: 22,716 checks. The preceding source reproduces the lost error display,
overflow while entering a decimal number, and invalid `INT64_MIN / -1`.

The final 6.5.217 kernel built without compiler/linker warnings, passed all 230
QEMU self-tests and reached login with four CPUs. In a separate 512 MiB,
single-CPU QEMU desktop, mouse/keyboard checks showed persistent division
errors, recovery to `7 + 2 = 9`, digit and addition overflow, Backspace correction
to `124`, and the full minimum/maximum signed values. These are VM checks;
hardware input and application fault isolation remain unverified.

## Limits

The application remains in kernel context. Checked arithmetic does not provide
application fault isolation, arbitrary precision, decimals, expression parsing
or a memory-safety proof for the desktop. It shares Hemera's keyboard mapping
and rendering; actual GUI checks are complementary to the host arithmetic test.
