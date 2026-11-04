# Assigned-function validation and shared build serialization

Status: current
Checked: 2026-11-04

2026-10-06

## Byte identity alone does not establish decompilation

A complete existing translation unit can pass the assigned function's byte comparison and
the whole-image check while still containing `INCLUDE_ASM` for that function. Those checks
correctly establish retail identity; they do not establish that its implementation is C/C++.
A candidate for `func_001B74E8` exhibited exactly this case and was rejected, not counted as
decompiled. Before accepting a candidate, also inspect that the assigned stub was replaced.
Neighboring stubs are legitimate when preserving a full existing translation unit.

Empty inline-assembly constraints or scheduling barriers are not readable decompilation.
Instruction allowlists alone cannot reject them because there is no instruction to inspect.
Reject that source pattern separately. Documented EE/VU instruction exceptions in WORKER.md,
including `sqrt.s`, remain permitted; this adds no exception to the source policy.

## Integrate into the existing translation unit

`func_00240358` matched as a standalone candidate but overlapped data owned by an existing,
wider unit. Promotion correctly refused the partial overlap. Integration preserved the full
`0x00240270–0x002421C0` unit and `.sdata=0x29DCC8:0x29DCE8`, replaced only the assigned stub,
and adjusted declarations and pointer argument casts to fit the decoder's understood types.
An independent `try_func.py` check reported **0 of 296 words differ** and **whole image: MATCH**.
The integrated source was then checked again and promoted.

`func_00134CC8` likewise passed **0 of 38 words differ** and **whole image: MATCH**, then promoted.

## Reports mutate the shared build too

`report.py` runs configuration, the main image build, and expected-object generation before
writing progress. It must participate in the same writer lock as promotion, not run alongside
it outside the lock. Its CLI now holds `srclock.exclusive()` throughout report generation.
Nested invocation from a writer inherits the existing lock rather than acquiring it again.

Following an earlier transient shared-build mismatch, a fresh configure/build and explicit
`check_image.py build/dhd.elf`, under the writer lock, matched all eight checked sections.
The mismatch was not treated as a successful check, and no retail comparison was weakened.

Operational checkpoints, recovery logs, provider details, and candidate artifacts remain
local-only. Current reported progress is **4,011 / 5,690 functions**, **825,916 / 1,405,096
matched bytes (58.780037%)**.
