# Late native-state nondeterminism

Status: current
Checked: 2026-11-04

The extended round 27 gate found one differing EE word at GO+60 seconds and
two at GO+75 seconds despite matching images and exact Z readbacks. Neither
word was excluded. A private hardware-write-watch build of the original shader
configuration reproduced the same late differing locations against the first
original-shader control; all four early state checks still matched.

## Producers identified

EE 0x2D5CC4 is rider owner 0x2CE200 + 0x7AC4. Watchpoints identify the store in
`func_001306C0`. This function declares `Vec out` without initialization and
unconditionally smooths `Dot(out.xyz, part->x38)` into `owner->x7AC4`, including
when the helper is not called. `func_001304F8` also returns before writing the
output on its first rejection branch, and writes only xyz before its short-
length rejection. Thus the trail-strength w input also has unwritten paths.
Retail assembly confirms these control-flow paths and stack reads: this is
not a shader result. Native host-stack contents cannot be treated as defined
game inputs or assumed to reproduce old EE stack contents.

EE 0x4C9B94 lies in sound command buffer D_004C9B00. The last diagnostic write
at field 7506 copies the x24 field of a 48-byte named-play request built by
`func_00201B68`. The payload copy in `func_00202048` is valid, but the producer
never initializes x24. Its sibling `func_00201BD0` has the same issue; the
union's unused half and trailing struct padding can also carry host data.
Subsequent smaller commands leave this old copied word in the command buffer.

## Port-only correction under validation

Define the rider's absent output as a zero vector before invoking the helper,
retaining all components the helper does write. This is an explicit native
choice for the inactive/unwritten case, not a claim of exact retail stack
history. Fully zero each named-play request before assigning its real fields,
including reserved bytes/padding. Keep matching `src/` and the retail build
unchanged by applying these fixes through `port/patches.py`.

Do not modify the EE comparator or add exclusions. Validate initialized
producer behavior, retain the old canonical early state/image checks, and
repeat the six-moment original/optimized comparison on the fixed build. If
the old reference changes, inspect the exact changed words before considering
a documented reference refresh. Acceptance is pending this work.

## Local evidence

`build/port/r27/gate-final/` retains the failed pair. The private write-watch
build and logs are under `build/port/r27/watch-state/`; `findings.txt` records
the native writer addresses, and `diagnostic-compare.txt` records comparisons
using the unchanged checker. A first unarmed startup was stopped to correct
capture configuration, marked `operator_aborted_setup`, and retained. The
corrected diagnostic captured all six moments; it is evidence of producers
and original-path nondeterminism, not a replacement passing gate.

## Early reference delta: a second full-vector producer

The three-producer fixed original/optimized pair matches all six EE captures
(ticks 0, 2, 12, 16, 3600 and 4500), all six race BMPs, four menu BMPs and
1,131 exact Z records through virtual GS field 9014. The unchanged old early
reference nevertheless fails with 7/9/6/8 words, all cleared to zero. That
failure remains recorded in `build/port/r27/gate-producer-fixed/`; it is not
relabelled a passing canonical gate.

A separate private original-shader diagnostic watches EE 0x2B348C from field
2800 to 3300. The native interpreter store at 0x4016918B copies a full quadword
to 0x082B3480; the trap follows at 0x4016918F. The actual caller return at
0x4003A168 is `func_00136038 + 0x1B8`: disassembly forms `o + 0x100` and passes
its local `force` vector to `COPY_VECTOR`. Thus the word is `o->x100.w` with
object base 0x2B3380. At clock 16 the watch reproduces the old reference value
0x3C404F24 exactly. Source assigns only `force[0..2]` before copying all sixteen
bytes. The other apparent addresses in the scanned native stack are stale
contents; a guessed trail-object/strength mapping is not evidence.

The fourth port-only patch explicitly assigns `force[3] = 0.0f` before that
copy. This defines an unused native lane, rather than relying on the earlier
output initializer incidentally clearing host stack storage. It does not
claim to reproduce retained retail stack bytes. The diagnostic and original
four snapshots remain under `build/port/r27/watch-early-delta/`.

`port/tools/test_defined_producers.py` compiles the four actual patched units
through the ordinary O1/SSE2 transformation and EE-float workflow. Across 256
stack-poison patterns it checks inactive, no-write, xyz-only and fully written
rider-helper paths; every byte of both 48-byte sound constructors in id/name
modes; and the gravity producer's complete copied quadword before its first
consumer. The latter preserves xyz and verifies positive-zero w. Consumers
are test stubs; the final gravity-body remainder is bounded with a test-only
longjmp. No matching source, comparison exclusion or reference is modified.

For a proposed reference audit, sound-request offsets are: bank union 0..7
(upper four reserved when an id is used), volume 8..11, pan 12..15, name
16..31, pitch fields 32..35, reserved `x24` 36..39, flags 40..43, and trailing
padding 44..47. A command adds a four-byte header. Only zero clears within
these verified reserved ranges, plus the proven force w lane, are candidates
for a documented producer reference refresh. Acceptance remains pending the
four-producer live pair and exact attribution of every changed word.

## Four-producer reference audit

The fixed-four control's early old-reference differences are 17/19/16/17
words, all cleared to zero. Ten per moment are the full-copy force w lanes
at `0x2B348C + rider * 0x7BD0`, for riders 0..9. For each lane, subtracting
0x10C gives its physics object's base; the captured object's pointer at +0x158
plus 0x40C0 equals that base. This verifies all ten instances of the same
producer, rather than guessing object type from the changed address alone.

The other changes map to the sound payloads below. Offsets are relative to
the 48-byte payload, after the four-byte command header. Only the listed
reserved ranges change. Existing captures or retained name/flag bytes prove
packet placement; overwritten headers are explicitly reconstructed, not
claimed as direct write-watch observations.

| Payload start | Changed offsets | Placement evidence |
|---|---|---|
| 0x4C8B08 | +0x04, +0x24, +0x2C | Intact named-play header in old tick 2 capture |
| 0x4C8B3C | +0x24, +0x2C | Retained `B_AI_ROCK_RIDE` name at +0x10 and flags 0x3D at +0x28; header overwritten |
| 0x4C8B98 | +0x04, +0x24, +0x2C | Intact 0x0030005C header and flags 0x3D |
| 0x4C9B08 | +0x04, +0x24, +0x2C | Intact named-play header in old tick 0 capture; later smaller commands retain the tail |
| 0x4C9B54 | +0x2C | Retained `P_DUM_USER09` name and flags 0x3D at +0x28; header overwritten |

The full 69 changed-word instances, old/new values and evidence classes are
recorded in `build/port/r27/gate-producer-fixed4/early-reference-audit.json`
and its Markdown table. The fixed-four versus fixed-three original-shader
states are not identical: they differ in 10/10/10/9/10/10 words across all six
moments, exclusively these newly defined force w lanes. That comparison is
retained as `fixed3-vs-fixed4.txt`; it is not called a passing equality check.
No reference refresh or comparison exclusion has been applied by this audit.

## Audited native reference accepted

The four-producer original/optimized pair matches all six EE snapshots under
the unchanged comparator, all six race BMPs and four menu BMPs byte for byte,
and 1,137 exact Z records through virtual GS field 9020. Both runs' old canonical
early race/menu images also match. The old EE reference failure, including
17/19/16/17 zero-cleared words, remains unchanged in
`build/port/r27/gate-producer-fixed4/result.json`.

After review of every changed word, the new native producer reference is
`build/port/compare/r27-defined-native/`. Its manifest links the fixed control,
source/object hashes and 69-instance audit. Rechecking completed artifacts
with the existing EE/readback functions produces a separate
`gate-producer-fixed4/refreshed-result.json` PASS; it does not overwrite the
old FAIL. No pointer range, screenshot exclusion or readback comparison was
added or relaxed. This refresh defines formerly unspecified native bytes;
it does not assert equality to retained retail stack contents or full-game
retail parity.

The gated executable is `producer-fixed4/dhd.exe`, SHA-256
`2eec2039ff8d6e624e34c15c15ba496afa5520ad27e7a4071f20870cf6020960`.
The accepted default-on executable changes only the GPU runtime object from
that frozen basis: `build/port/r27/accepted/dhd.exe`, SHA-256
`7cef7b180664fa5e9d378913f8b0363b558e04f1dce5e2ab7e182abbfaf9a411`.
Its manifest retains the four O1/SSE2 producer object hashes and unprofiled
runtime settings. One expanded WARP suite with all three shader environment
options absent passes with zero failures. The matching retail build was
separately rechecked by the lead: all eight sections MATCH.
