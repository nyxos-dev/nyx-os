# Selene tab memory

Development version 6.5.203 defers the seven 1200-by-96 page grids until a tab
first renders content. A blank tab still owns its context, links, fields, forms
and image metadata. The initial window renders the local welcome page, so it
still allocates those grids; the saving applies to additional blank tabs.

Grid allocation is all-or-none. A failed first allocation releases every
temporary grid and leaves the tab available for retry. Required renderer scratch
allocation failures now report `Out of memory: page not rendered` instead of
reporting a successful load. Existing grids are reused on subsequent renders.
Closing a tab frees its metadata and any allocated grids.

## Verification

`python3 tools/test_selene_memory.py` compiles the actual browser implementation
with ASan/UBSan. Its 358 checks cover constructor/tab allocation failures, 100
grow/shrink cycles, seven grid failures, eleven required renderer scratch failures,
retry, optional mapping failure, local startup and a small form fixture. Tracked
payload returns to zero after each tested lifecycle. Exact host payload sizes are:

| Workload | Bytes |
|---|---:|
| One blank tab and its manager | 121648 |
| Six blank tabs and their manager | 729608 |
| One rendered tab and five blank tabs | 1536008 |

Each additional blank tab avoids 806400 bytes of grid allocation. The same
six-tab allocation baseline in 6.5.202 used 5568008 bytes. These host payload
counts exclude allocator headers, fragmentation and the rest of the OS.

The candidate builds without warnings, passes 229 kernel self-tests and reaches
login with four QEMU CPUs. A separate one-CPU, 512 MiB QEMU desktop check records
3450 KiB of heap before opening Selene and 4951 KiB with its local welcome page
and five blank tabs, compared with 8890 KiB for that workload in 6.5.202. These
are rounded whole-system heap counters, not exact per-browser allocation counts.
Find, scrolling keys, tab switching/closing and blank-page clicks remain responsive
in that session. Closing the window returns the counter to 3450 KiB with zero
allocation failures and intact task kernel-stack canaries. This is one bounded
interaction sequence, not a long-running stability measurement.

The older full HTML/form self-test reports 10/14 passing in both preserved
6.5.202 and candidate host builds, with byte-identical diagnostics and zero
remaining tracked payload. Entity, nested-table, nested-list and blockquote
cases remain baseline failures in that host check; this memory regression does
not claim to repair them or establish their target behavior.

## HTML/form test maintenance (6.5.204)

An additional 6.5.203 QEMU run reproduced the same four failures. Comparing the
original fixtures in 202 and 203 showed identical rendered bytes: `&nbsp;` uses
non-breaking byte `0xFF`, unordered lists use bullet `0xF9`, blockquotes use a
`0xB3` bar and three spaces, and nested tables keep their rows on separate aligned
lines within the outer cell. The old assertions expected earlier representations.

Development version 6.5.204 aligns those four assertions with the implemented
contracts. It retains exact content, indentation, adjacent table-row and border
checks. All twelve render calls in `formtest` now detect required allocation
failure, free the context and return failure rather than inspecting an incomplete
fixture. The browser's renderer and navigation are unchanged by this increment.

`python3 tools/test_selene_forms.py` runs the actual 14-case suite with ASan/UBSan,
injects failure at each of its 205 allocation sites and verifies cleanup, then
checks a successful retry. Its 207 normal assertions pass. Four separate mutated
renderer builds deliberately replace the non-breaking space, bullet, quote bar or
nested-cell content; each is rejected by the corresponding assertion. The preserved
203 implementation fails the new regression with the original 10/14 result.

The 204 build has no warnings, retains all 358 memory checks, passes 229 kernel
self-tests and reaches login with four CPUs. Its one-CPU, 512 MiB diskless QEMU
desktop runs `formtest` to 14/14; heap use is 3450 KiB before and after, peak
4713 KiB, allocation failures zero, and `stackcheck` reports intact canaries.
The terminal remains responsive and the VM closes. Allocation injection and
mutation sensitivity are host evidence, not fault injection in the target VM.

## Remaining limits

Network requests, including image requests from the compositor tick, remain
synchronous and can delay input. This change does not establish a fix for all
reported browser freezes. Image decoding still needs a measured aggregate memory
budget. The kernel heap capacity is unchanged. A scratch allocation failure can
clear the prior rendered document; preserving a page transactionally is separate
work. No remote-navigation, soak-test or physical-hardware qualification is
implied by the local checks above.
