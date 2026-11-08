# Shared animation and vector headers

Status: partly superseded by 088 (x87 return checks of the i686 build)
Checked: 2026-11-04

The shared headers and every source fix are current. The x87 float-return checks and "native
i386" observations describe the retired 32-bit build (finding 088); on the wasm32 core float
returns are checked by `abi_check.py` and the scalar/float ABI tests (`port/tools/test_float_abi.py`,
`scalar_abi.py`; finding 104).

2026-10-13. Both builds already search `include/` (SN through `tools/sncc.py`,
PC through `port/tools/build.py`). No compiler flags or match checks changed.

## Coverage and approach

`animation.h` declares one 16-byte AnimationHandle: owner/track pointers,
word +8, signed shorts +C/+E. It shares the observed owner, range, range-table,
track and track-list prefixes, 22 entry points and the float handler type.
These are prefix layouts, not reconstructed full object sizes. `vector.h`
declares one 16-byte aligned Vector4 and four-row Matrix4, plus typed
00221888/002226C8 prototypes. Size assertions protect Handle/vector/matrix.
All API declarations have C linkage in C++.

Six animation units converted: `0015A7C0.cpp`, `0015B018.cpp`, `00179338.cpp`,
`00202E00.cpp`, `00206320.c`, `00206E48.c`. The merged unit replaces 19
private Handle views (00203848 through 00204230, plus 00204450 and 00204770)
and their dependent prefix types/API aliases.

Eighteen vector units converted (all `.c`): `0010AFE0`, `0010B2B8`,
`0010B998`, `0010BF50`, `0010DEF8`, `0010E420`, `0010E6C0`, `0010E9B0`,
`0010EC80`, `0010ED50`, `0010F0D8`, `0010F710`, `0010F8E8`, `0010FB00`,
`001101B8`, `001304F8`, `00221888`, `002226C8`. The trail helper shares
types but retains its aggregate signature and port patch.

## Real disagreements fixed at source

Evidence: matching callee bodies and retail asm in `asm/nonmatchings/text/`.
Handle leaves read 16 bytes through a0; 00203B68 receives apply in a1 and
time in f12; 00206E48 preserves a0/a1/f12 through jalr and returns f0.
Existing port repairs and real caller regressions corroborate these shapes.

| Function / declaration | Wrong native form | Shared form |
| --- | --- | --- |
| 00203848, 00203C28 definitions | float return, Handle by value | `float (const AnimationHandle *)` |
| 002038E0, 002039B0, 00204080 definitions | Handle by value, int | `void (const AnimationHandle *, int)` |
| 00203CB8, 00203EF0, 00206320 definitions | Handle by value | `void (const AnimationHandle *)` |
| 00203C28/00204230 in 0015A7C0 and 0015B018 | Value by value | shared Handle pointer; float/int returns retained |
| 00203B68 in 00179338 | Sound by value, int, float | `void (const AnimationHandle *, int, float)` |
| 00203848, 00203CB8, 00203D18, 00203F40, 00203FE0, 00203A80 calls in 00204770 | Handle by value | shared pointer prototypes, explicit copies |
| 00206E48 and Handler | `void (unsigned short *, int)` | `float (unsigned short *, int, float)` |

Explicit local copies preserve the original EABI aggregate copy instructions.
Pointers to the local Handle retain its address calculation/register allocation.
The two query callers require separate progress/status temporaries; 00204770
requires a fresh copy before each call. Directly passing the original object
changed bytes. Track +29 must remain unsigned (`lbu` in 00204770).

## Exceptions and next round

- `00126B20.cpp`: unifying Sound and its long selector changed registers and
  size. Restored the unit and retained its port patch.
- `00221608.c`: correcting int/float order swapped two instructions in this
  -O0 unit. Restored it and retained its audited PC permutation.
- `001304F8.c`: explicit vector input copies changed 112/114 words. Shared
  types match, but its aggregate parameters/PC pointer-input patch remain.
  No canonical declaration for this exceptional entry was added to the header.
- `func_00203D18__c14` retains a local extra owner argument to preserve retail
  a1 setup; the shared leaf reads only a0. Commented beside the declaration,
  and not counted as a fixed disagreement.
- Packed Handle, constructor/output and transition-node views remain local.
  Remaining port regexes require exactly 1 definition and 10 declarations.

Next: finish those views and 00203590/002036C8 output types, including the
12-byte 001B7198 local; unify the x792C rider/body prefix and 00212048 iterator;
then expand vector/math and highest-call-count prototype coverage.

## Port integration and verification

Removed five patch entries: 0015A7C0, 0015B018, 00179338, 00206320, 00206E48
(11 text replacements). Merged-unit aggregate regex counts changed 8/24 to
1/10. Unchanged ABI guards for 0010ED50 and 00202E00 were rebased to new lines;
hashes/arguments/permutations remain intact. Real Handle regressions now use
shared pointer signatures and check all 16 bytes, including +E. Synthetic
fixture symbol renaming includes the header declarations. Another worker's
temporary PC animation diagnostics are excluded from these commits.

- `python tools/configure.py` and `python -m ninja`: MATCH, all eight sections.
- `python tools/check_image.py build/dhd.elf`: MATCH, all eight sections.
- Targeted `python port/tools/build.py -j 16`: 24/24 compile, zero errors.
- `python port/tools/test_runtime.py`: passed, including real Handle caller
  regressions, integer/RPC/dispatcher checks, callbacks, VU0/VU1, GS/VIF,
  controller/timer/SIF, sound and D3D11 WARP.

Research: reviewed public [ps2dev EE toolchain configuration](https://github.com/ps2dev/ps2toolchain-ee/blob/main/config/ps2toolchain-ee-config.sh)
and [ps2sdk build definitions](https://github.com/ps2dev/ps2sdk/blob/master/Defs.make).
They concern the current homebrew toolchain, and were not used to establish
SN 2.95 codegen or game signatures. No external code or game data imported.

## Round 2: constructor outputs (2026-10-13)

Eleven units now share the 00203590/002036C8/002037D0 output prototypes:
0012BF88, 001418F8, 001AC418, 001ACCC8, 001AF8D0, 001AFAA8, 001AFE00,
001B00C8, 001B7198, 001E7520 and 00202E00. The output is AnimationHandle;
its +C/+E are signed shorts, not a float or one int. 001418F8's Packet and
001B7360's local were 12 bytes; both are now the full 16-byte Handle. The
constructor writes through +C (14 bytes), and subsequent leaf copies read
all 16 bytes, so a 12-byte allocation was invalid even though EE stack
padding hid it. The +E word is not initialized by these constructors.

002036C8's first selector is an integer (compared against packed bits >>18),
not a pointer. Corrected pointer-typed x796C/x7970 selector views in
0012BF88, 001ACCC8 and 001AFE00. 001AF8D0/001B00C8's float-ending outputs
and 001E7520's untyped outputs now use the same Handle. Shared AnimationOwner
also exposes the proven +0/+C/+10/+1C prefix fields.

Removed the 001B7198 local-size patch from port/patches.py; that file already
contains the concurrent port worker's edits and is deliberately not staged
in the shared-header commit. All eleven touched PC units compile (0 errors).
Configure/build and the separate check_image invocation both print MATCH
for all eight retail sections. No check or compiler flag changed.

Remaining Handle pass: 002031E0, 00203380, 002033E0 and 002042F0 in the
merged unit now use the shared Handle and canonical declarations. 00203380
and 002033E0 formerly took aggregates by value; they now take a pointer and
make the retail local copy. 002033E0's source lives in Handle +8, unlike
002031E0's track at +4; it remains an explicitly cast transition-node view.
001AC680/001AC810 now retain the callee's signed-short priority and pointer
return (previously int priority/void return). 001AE258/001B11C8 share the
output Handle and transition API; 001B11C8's selector is int, not void *.
00162FB0 shares the range/transition calls, and both 00205D10/00205F40 scratch
Handles are canonical. All six units compile for PC; eight-section MATCH.

Exceptions tested and restored: changing 00202E38's aggregate leaf calls to
explicit copies changed 34/231 words. 001AC620/001AC7B0's float-vector copies
and extra unused float argument must remain (23/24 words changed); doing the
same in 001ACA08 changed 26/28 words and grew its body 0x70 -> 0x7C. They
retain local declarations, including the extra reset float, rather than
changing the match. The merged-unit port regex now repairs seven dispatcher
declarations only; no aggregate definitions remain. The unchanged guarded
00206E48 call was rebased by its exact hash; port/ files stay unstaged for
the concurrent worker. No game data or external code imported.

## Round 2: rider/body render prefixes

`include/rider.h` shares SceneNode (position +10, Matrix4 +40), Rider
(bodyNode +792C, integer riderIndex +797C), and RiderComponent (rider +16D0,
node +16D4). Offset assertions compile with both SN and clang i386. Rider is
explicitly a prefix, not an array stride/full object-size claim.

Eighteen units converted: 001AC578, 001AD098, 001AE8A8, 001AE9B0, 001AEA30,
001AEB20, 001AEB88, 001B0C10, 001B0D38, 001B0E00, 001B1C08, 001B31A0,
001B3300, 001B33A8, 001B3AE0, 001B3E98, 001B3F48 and 001B6668. The xA10
and xC0 chains now lead to the same RiderComponent/Rider/SceneNode layouts;
their enclosing camera/attachment views stay local.

Every disagreement encountered in this render batch:
- x792C was char *, void *, a partial Target/Frame/Body/Pose, or a Node;
  it is one SceneNode pointer. Rider/body names were sometimes reversed
  across the +16D0 link; those links now share RiderComponent.
- SceneNode +40 was one vec4 (001AEA30/001B31A0), three rows (001B3F48),
  or four rows (001AC578/001AD098/001AE9B0/001B6668). The four-row copy in
  001AC578/001B6668 and 001AE9B0 proves Matrix4, with 16-byte alignment.
- 00221888 had float *, Vec4 * or void * matrix parameters; all touched
  callers now use the shared Matrix4/Vector4 declaration.
- 001AE8A8's fourth argument was a Vec4 * in 001AEA30, but the callee
  writes 64 bytes; the shared declaration requires Matrix4 *.
- 001B33A8 declared x797C and 0023AEB8's input as pointers. 001B1C08
  multiplies that field by 26, and 0023AEB8 uses it as the same integer
  rider index; both declarations are now int. The older 0012BF88 low-half
  unsigned-short access remains local and is not claimed to be unified.
- Position was u128 in 001B0D38, float[4] in 001AC578/001AD098/001B3F48,
  or aligned Vec4 elsewhere; these views now share Vector4.

All 18 PC units compile, zero errors. Configure/build and separate
check_image both MATCH all eight sections. No retail-sensitive exception
was needed in this batch; no port patch removed. Layouts and names come from
the matching USA bodies above.

## Round 2: spatial iterator

`include/spatial.h` settles the iterator at 0x70 bytes, aligned to 16:
two bounds vectors +0/+10, six cell indices +20..+34, a 0x20 cell cursor
at +38, an eight-byte filter at +50 within that cursor, a distinct transform
reference at +58, collider pointer +60, matrix pointer +64, and two unsigned
short tags +68/+6A. Size and offset assertions cover the layout. The 00212048
0x60 view and 00212098/00212408 prefixes were incomplete, not different
iterator objects. 002121E0 passes &collider to a result writer which fills
the contiguous +60..+6A result; &matrix is also the reference's matrix node.

Seventeen units converted: 00210950, 00210A18, 00210CA8, 00211780, 00211980,
00211BE0, 00211D68, 00211DF0, 00211EC8, 00212048, 00212098, 002121E0,
00212408, 00212738, 00214A18, 00214BA0 and 00236EF8. Filter, cell cursor,
reference, collider, hit and spatial actor prefixes now have single shared
types. The actor's 64-bit flag field uses mode(DI) so the header retains
EE width on i386. Signed casts before two flag shifts retain the original
sra instructions without changing their masked single-bit result.

Disagreements fixed:
- 00212048: void vs iterator-pointer return in 00214BA0, now pointer.
- +58: filter/Pair vs reference/Obj; now a matrix-pointer reference plus
  optional actor. +60: int vs pointer; now SpatialCollider *.
- 00210950: int definition vs pointer callers; now SpatialFilter *.
- 00210A18: (hits, capacity, radius, t0, t1) vs
  (radius, t0, t1, hits, capacity); every touched caller uses the latter,
  the callee's actual definition. Both integer and float bank order match.
- 00210CA8: shared definition keeps radius before start/end, then t0/t1,
  Matrix4 *, flags, hits, capacity. 00214BA0's matrix was called an int
  and its args were shuffled; canonical reordering changed five retail
  instructions, so its documented __retail alias and PC permutation remain.
- Hit +18/+1C scalar views are the third/fourth floats of the +10 vector;
  all touched hit consumers now use the same 0x30-byte SpatialHit.

Retired three 00210A18 call permutations from port/abi_calls.json. The
00214BA0 -> 00210CA8 exception was regenerated with clang's AST audit against
the actual callee, retaining its required permutation and source guards.
The existing 00214BA0 quadword-output port patch remains. These port edits
are unstaged for the concurrent port worker. All 17 PC units compile with
zero errors; configure/build and separate check_image MATCH all eight sections.

## Round 2: broader vector/matrix coverage and review handoff

`vector.h` now declares 00221930/00221A60 matrix products, 00221F20 identity,
and 002224E0 Matrix4 -> Vector3 angles. Vector3 is explicitly 12 bytes;
the angle query writes three scalar floats, unlike a quadword vector writer.
Eleven units converted/extended: 00221930, 00221A60, 00221F20, 002224E0,
00215140, 00215448, 00236EF8, 001AEB88, 001B3440, 001EA218 and 001EA918.
All eleven PC units compile, zero errors, and all eight retail sections MATCH.

The shared matrix signatures replace float *, void * and vec4-array views.
001EA218's x792C was a Matrix * with `+1` meaning byte +40; it now names the
SceneNode and selects its matrix explicitly. 001EA918's char * +40 is the
same typed matrix field. 001B3440 uses the shared SceneNode/RiderComponent
while retaining its larger local World overlay for fields not yet in rider.h.
No matching-sensitive exception arose in this batch, and no port patch
was removed. All commits have the requested coauthor trailer.

Round 2 totals: 60 distinct touched units, including the merged animation
unit already converted in round 1. Verification was repeated before every
commit: configure + ninja, then a separate check_image, all eight sections
MATCH. Every touched unit was compiled with the native i386 port build.

Port handoff (working-tree edits, not included in source/header commits):
remove 001B7198's local-size patch; keep only seven dispatcher declaration
regex repairs in 00202E00 (no aggregate-definition regex); retire the three
00210A18 permutations in 00211980/00211BE0/00214BA0; retain the regenerated
00214BA0 -> 00210CA8__retail permutation. Unchanged guarded sites in
00162FB0 and 00202E00 need their current source lines (hashes preserved).
These files have concurrent port-worker edits, so only owned source/header
and finding/state paths were staged. No runtime/race improvement is claimed
from compile/matching checks alone.

Next review plan: extend the rider prefix to the remaining rich overlays
and array users only with the proven 0x7BD0 stride; broaden shared Handle
callers (especially 00202E38); investigate the retained local forms;
then unify 00212488/00212500/002125A8/00212688 spatial transform signatures
and their vector/matrix buffers. Do not remove their port fixes until the
same retail and native checks pass.

Final reset-call audit: 00203380's local 00206D18 declaration was
`void (void *)`, dropping the node still in retail a1. The callee walks a
linked list and compares each node against a1, then returns int. Corrected
the local alias to `int (void *, void *)` and passed the output node p.
The whole image still MATCHES; this fixes another native missing argument
without a port patch. The broader linked-list type/API remains future work.

## Round 3: commit boundary repair

The round-2 port handoff is now staged with a patch built from HEAD, excluding
the concurrent worker's animation diagnostics, 00177DA0 sprintf repair and
libc_shims.c. Retired 001B7198's stale local-size patch and the obsolete
00202E00 aggregate-definition regex; its declaration count is seven. The
ABI manifest contains only the previously documented round-2 removals,
regenerated exception and guarded line rebases. Six affected PC units compile
using the staged patches.py blob (not the working-tree diagnostic version),
with zero errors. Configure + ninja and separate check_image print MATCH
for all eight sections. Future source changes and their port edits are committed
together using the same staged-blob boundary when files are shared.

## Round 3: spatial transform API

Twelve units converted/extended: 001AEED0, 002092B8, 00209300, 00209460,
00210A18, 00210CA8, 00212488, 00212500, 002125A8, 00212688, 002219A8
and 00236E20. spatial.h owns the four reference-transform APIs and the
moving-transform sampler/velocity APIs. SpatialMotion's +10/+90 positions,
+40/+C0 matrices and +B0 duration come from the sampler/velocity bodies;
three offset assertions protect the shared prefix. The static reference
points to Matrix4 *, while its moving actor points to SpatialMotion.

Disagreements fixed: 00212488's second argument was int but is Matrix4 *;
00209460/002219A8 were falsely declared to return int there, now void.
00212500/002125A8 represented the moving actor as int, now a typed pointer.
All four reference-transform definitions/callers share Vector4/Matrix4
inputs/outputs rather than char/float arrays or void pointers. The 16-byte
position and 64-byte matrix scratch buffers are explicitly aligned types.
001AEED0 also shares the rider/body chain and its +40 full matrix.

All twelve PC units compile, zero errors. Configure + ninja and separate
check_image print MATCH for all eight sections. No permutation or port patch
was retired here; the hand-written 002219A8 native-body patch remains.
Evidence is the existing matching USA definitions/retail assembly; no
external code imported. Remaining old matrix-helper declarations in
00209300/00209460 will be addressed in the high-call-count API pass.

## Round 3: rich rider overlays and stride

Sixteen units converted: 001A3220, 001A8830, 001A8EF0, 001A9058, 001A9268,
001A93B8, 001AD2B8, 001ADBC0, 001ADE98, 001AEC60, 001B0320, 001B0F80,
001B1098, 001B1DD8, 001B3440 and 001BE328. Rider now includes known control,
physics, fade, result and animation fields with padding to the proven 0x7BD0
array stride (001A3220 and other existing array users). This establishes
sizeof/stride, not meanings for the remaining padded regions. RiderPhysics
shares the node +38, velocity +C0, state +160 and short +4A4 views; offset
assertions cover their positions within Rider. Body +50/+60 vectors are
rows one/two of the shared +40 Matrix4.

001AD2B8's generic four-int items passed to 00203B68 are AnimationHandles.
Both Rider's initial array and RiderComponent's +14 array now use that type;
the temporary and call declaration are canonical. Its enclosing +7A78
float stays offset-named. 001AEC60's raw +16D0/+792C chain is typed and its
+1D60/+1E70 probes are shared vectors. Its bone matrix starts at +10; the
old local view described only 44 of the 64 bytes written by 00221F20, now
a complete Matrix4. This is an object prefix, not a stack-allocation claim.

Other disagreements removed: SceneNode +18 scalar "time" views in the
recorder units are position.z; rider +797C is the same integer index in
001B0F80/001B1098/001B1DD8. 001B3440's rich World and 001ADE98's Rider
are replaced by the shared layout. All added header offsets and the rider
stride compile under SN and clang i386. Aligned component probe fields
round its prefix extent up to +2170; the following rider padding accounts
for this, preserving every asserted retail field address.

All sixteen PC units compile, zero errors. Configure + ninja and separate
check_image print MATCH for all eight sections. No port patch/manifest
change was needed. Existing source assembly helpers were preserved.

## Round 3: remaining Handle queries and dispatcher callers

Twelve units converted/extended: 0013C400, 001401B0, 00170E80, 00181CB0,
001ACBA0, 001E7E80, 001E8440, 001E86E8, 001E8B70, 001E9250, 001E9628
and 00202E00. The 00204280 definition now uses AnimationHandle, AnimationRange
and the shared float-returning prototype. Caller "Vec"/four-word Handle
views are canonical in the six rider animation state units. The new query
retains its float return. 0013C400's 12-byte sound local is now 16 bytes;
00203590 writes through +C and later leaf copies read all sixteen bytes.

00202E38's true argument order is (Handle *, enable, mode, float amount).
Thirteen of twenty-two PC permutations are retired: the affected source
calls now use that shared prototype and order. Nine calls in 00170E80,
001E8440, 001E86E8, 001E8B70, 001E9250 and 001E9628 require explicit
__retail aliases with float before the two ints: canonical ordering changes
2-3 setup instructions per sensitive call. Their typed, source-guarded
permutations were regenerated with clang AST inspection against the actual
00202E38 definition. Unchanged sites in edited units were rebased only after
checking the original fragment/hash. The merged dispatcher itself retains
its round-2 aggregate exception; its seven declaration regex repairs remain.

00204230's bool caller declarations are now the shared int return. Since
the matching leaf returns exactly 0/1, xor with one expresses the stopped
flag while retaining the retail xori/andi sequence. Casting its result to
unsigned char before logical negation instead shifted those functions.

All twelve PC units compile using the committed patches.py blob, zero
errors. Configure + ninja and separate check_image print MATCH for all
eight sections. Source and manifest changes are committed together; no
concurrent diagnostics were staged. Next priority within the render path:
001B1698 (58 reported return disagreements and 24 audited permutations),
then its adjacent controller setters and the broader ABI census.

## Round 3: high-call-count rider blend/tween API

Twenty-three units converted/extended: 001288B8, 00152768, 00152C58,
00152FF8, 00153168, 00153938, 00153A30, 00162FB0, 00163148, 00170E80,
00171050, 001A4CE0, 001A4E08, 001A4F70, 001AE630, 001B1698, 001B3170,
001BB9E0, 001E9CC0, 001E9F10, 00208660, 002086C8 and 00208708. This
selects the highest-count remaining flagged setter in the rider path: the
prior ABI report had 58 false void-return calls to 001B1698 and the manifest
held 24 argument permutations.

include/tween.h shares FloatTween (float *target, start/delta, tick/duration)
and RiderBlendController's +40 weight/+80 tween prefix, with size/offset
checks. The 00208708 body proves the destination is a float pointer: it
writes the interpolated scalar through +0. 00208660 and 002086C8 formerly
called that destination and the tween pointer ints; their API is now
int (FloatTween *, float *, int, float, float). 001AE630/001B3170 now use
the same callee declaration and full embedded tween outputs; their own
void wrapper definitions remain void because they discard the callee result.

001B1698 returns the integer registration result from 002086C8. Its shared
prototype is int (RiderBlendController *, int frames, float weight),
replacing callers' void return and swapped float/int parameters. All
eighteen definition/caller units use the header. Five permutations retire;
nineteen retain explicit __retail aliases to preserve observed scheduling.
The aliases return int except two final calls in 001A4F70/001A5218 and one
in 001BB9E0: their unused return must retain the void call form to preserve
register/store scheduling. These three exceptions are documented and stay
visible in the scoped clang ABI report; 55 of the original 58 return
disagreements are removed. Swapping the independent source stores in
001BB9E0 did not reproduce retail and was restored.

All twenty-three PC units compile, and a final pass compiles all 62 distinct
round-3 units with the committed patches.py blob, zero errors. Configure +
ninja and separate check_image print MATCH for all eight sections. Guarded
manifest changes are staged separately from concurrent edits and committed
with their source. No port diagnostics, local tool settings, proprietary data
or keys are included.

Next review plan: shared 001B3170/001AE630 wrapper APIs and their retained
permutations; larger overlapping rider/Handle array views (the current
shared arrays describe the proven initial 50 entries only); 00202E38's
aggregate dispatcher exception; then remaining ABI targets by call count.
The rider's 0x7BD0 extent is checked, but its padded regions and wider
Handle array limits still require evidence before replacing other overlays.

Final round-3 runtime verification: python port/tools/test_runtime.py exits
zero, including real Handle/rider callers, callback ABI/stale-site checks,
VU0/VU1/GS/VIF, controllers/timers/SIF, sound and D3D11 WARP regressions.
This ran with the current shared working-tree port files; the separate
62-unit compile used committed patches.py to verify the clean-checkout
patch boundary. No rider/bike visual improvement is claimed without a
subsequent race capture.

## Round 4: adjacent blend setters and progress baseline

Fresh full clang ABI audit of HEAD (using HEAD's patches.py, excluding concurrent
diagnostics): 1,634 warning flags, 162 guarded permutations. The prior generated
report was stale. Source-site guards were evaluated against a HEAD source snapshot,
so edits made during inspection could not inflate unavailable-range warnings.
The tool/checks are unchanged; signed/unsigned TI disagreements remain counted.
HEAD carries 47 text patch operations. Round-1 through round-3 removals remain
six complete PATCHES entries (five in round 1, 001B7198 in round 2) plus the
obsolete merged Handle-definition regex; round 4 starts with zero new removals.

Twenty-seven units share the adjacent 001B3170/001AE630 APIs and their checked
RiderSecondaryBlend/RiderBodyBlend prefixes in tween.h: 001288B8, 00152768,
00152C58, 00152FF8, 00153938, 00153A30, 00162FB0, 00163148, 001707C8,
00170E80, 00171050, 00181CB0, 00181D60, 00184470, 001845A8, 001A4CE0,
001A4E08, 001A4F70, 001AE318, 001AE630, 001B3170, 001BB940, 001BB9E0,
001DBCE8, 001DBE38, 001E9CC0 and 001E9F10. Both true signatures are
void (typed object *, int frames, float weight). Local float/int and
float/object/int declarations are replaced with canonical APIs or typed,
documented retail aliases. The embedded outputs are full FloatTweens at
+F8/+88, with destination weights at +F4/+40; offsets are asserted.

Canonical ordering changed 35 retail words in nine units (ten functions).
All sixteen affected call permutations must retain the original setup order;
their explicit __retail aliases and AST-generated guards remain. The wrappers
discard the registration result, so their void return is correct. Twenty-seven
PC units compile with HEAD's committed patches; configure + ninja and separate
check_image both print all-eight-section MATCH. No checks/flags were weakened.

Running round-4 count after setters: flags 1,634 -> 1,634; guarded permutations
162 -> 162; text patches 47 -> 47; complete PATCHES entries removed this round 0.
Next: wider rider Handle array evidence, dispatcher aggregate exception, then
highest-count ABI targets. The first full audit ranks 00201B68 (267 warnings),
001DB4C0 (102), 00233BA8 (65), and 00201BD0 (48). Most of the first two are
signed/unsigned TI spelling differences; two 00201B68 callers also have a real
64-bit bank-selector disagreement. Shared declarations will distinguish these.

## Round 4: Handle allocation, proven array bounds and dispatcher layout

Ten units converted/extended: 0013B5E8, 0013BA60, 0017FB18, 001825B0,
0018B3A8, 001AC418, 001E7E80, 001E9628, 001F87B0 and 00202E00.
Six lookup-local patches retire with their source changes: the first five
lookup units above plus 001F87B0. All their outputs now use AnimationHandle;
the two locals in 0018B3A8 also share the API, replacing four incorrect int
returns for the void constructor/reset. Its unrelated 12-byte records stay
private. The 0013B5E8 global used by the lookup is a Handle too.

Rider and RiderComponent now contain 254 Handles. Evidence: 0012BF88 fills
rider indices 1..253; 001AC418 fills 253 component slots starting at +24,
after slot 0 at +14. Component end is +FF4, where the distinct +60-byte
object begins; the former 256-slot state view overlapped that object. The
484-slot rider view in 001E7E80 overlapped the established +1190 fields.
Both constructors' loops and layout assertions agree on 254. Shared prefixes
also expose component +211C, sub6610 +4/+10E6, rider +78D4 and +7908.
Rider remains exactly 0x7BD0. The array users share the headers but retain
byte-addressed Handle-copy expressions: typed indexing changed 197 words
by replacing retail unaligned ldl/ldr copies. Restoring those expressions
with the shared types restores every byte; no PC permutation is added.

00202E38 now defines the canonical shared pointer API and uses AnimationHandle
instead of its private Arg layout. Its owner/track special views are explicit
casts where the transition code uses the longer object prefix. Seven leaf
aggregate aliases remain: both direct pointer calls (round 2) and new inline
value wrappers differ by 34/231 words. The declaration-only PC regex stays
at seven. No unsupported asm/scheduling tricks were introduced.

All ten touched PC units compile. All forty current rider.h users also compile;
configure + ninja and separate check_image print MATCH in all eight sections.
Running round-4 flags: 1,634 -> 1,630; permutations 162 -> 162; six complete
PATCHES entries/six text operations removed this round (twelve complete
entries cumulatively). Task-owned text count is 47 -> 41. Concurrent renderer
cleanup independently removed two text operations, so the actual staged count
for this commit is 39. Owned patch hunks are applied to the index separately.

## Round 4: highest-count sound API and emitter layout

Sixteen units converted: 001C33D0, 001C3428, 001C3740, 001C5040,
001D53C0, 001D5C30, 001D6390, 001D6D68, 001D7968, 001D8618,
001D8F18, 001D9038, 001D9A40, 001DB4C0, 001DB510 and 00201B68.
sound.h owns the named-play API, the 64-bit bank name, 128-bit sound name,
0x30-byte SoundPlayRequest and 0x40-byte SoundEmitter. DI/TI mode attributes
keep both integer widths under SN and clang i386. Size/offset assertions
cover name +10/flags +28 in the request and name +20 in the emitter.
The emitter's +10 position now shares Vector4; both initializer/update use
the same complete structure, including the secondary handle and distances.

00201B68 callers formerly declared the bank-name input int (32 bits on PC)
against the callee's EE long (64 bits), and used unsigned TI sound names
against a signed TI definition. Two merged callers still had a real 32-bit
slot after the port's declaration repair, because their first argument was
also wrongly pointer-typed; those bank ID globals are now int. All callers
use the single shared declaration directly. 001DB4C0's generic object pointer
and signed TI name are now SoundEmitter * and the same unsigned SoundName.
The checker counted 267/102 disagreements respectively; all 369 disappear.
Most were signedness/type spelling, not different-sized runtime slots.

Reviewed the public [989snd EE implementation](https://github.com/Ziemas/989snd/blob/master/ee/989snd.c),
especially the named-play VolPanPMPB interfaces and RPC callback widths.
It corroborates the bank/sound-name widths and volume/pan/pitch field names;
retail matching bodies and existing request-byte regressions establish this
game's layout. No external code or proprietary data was imported.

Configure + ninja and separate check_image print MATCH for all eight sections.
All sixteen touched PC units compile. The existing integer/RPC regression
fixture now expands sound.h before its vector dependency, so it tests the
real shared-header producer and full four-word sound name. No port patch
retired in this step. Running round-4 flags: 1,634 -> 1,261 (369 here, four
in the lookup step); permutations 162 -> 162; six complete PATCHES entries
and six text operations removed by this worker. Next highest flagged target
is 00233BA8 (65 incorrect void returns), followed by 00201BD0 (48 retained
callback/return exceptions) and 00236B28 (27 callback registration returns).

Runtime suite exits zero after updating both affected fixture preparations for
the shared headers. Request-byte/name-width, real Handle callers, callback ABI,
GS/VU1/VIF, controller/timer/SIF, sound and D3D11 regressions pass. Optional
VU0 microcode comparison was not enabled in this invocation. The rider-time
negative fixture still deliberately allocates the former 12-byte lookup local;
--unpatched-handle trips the stack protector (exit 0xC0000409), while the real
shared-header source reports zero failures. Its source regression remains
meaningful after retiring the corresponding port patch.

## Round 4: shared event queue and final verification

Twenty-six units converted: 001BC968, 001BCAD0, 001BD890, 001BDA58,
001BDDA0, 001BE778, 001BE8B0, 001BEB50, 001BECD0, 001BF0B8,
001BF218, 001BF7E8, 001BFD50, 001BFEA0, 001C0BF8, 001C0F88,
001C1170, 001C1300, 001C1420, 001C1548, 001C1650, 001C18A0,
001C1C00, 001C1F50, 002337C0 and 00233BA8. event_queue.h shares the
queue class/method form, 0x10-byte item prefix, 0x18-byte info prefix and
0x10-byte table prefix. The two implementation units use the same class
and records; the C-facing API has the explicit object pointer. Virtual order
and member order are retained, without claiming SN/native class-layout parity.

00233BA8 is int (EventQueue *, kind, index, int *data, byteCount, link),
replacing all twenty-four caller units' void returns and generic payload
views. Its body returns the index on failure as well as success; the
registration result must remain int. All 65 warning flags disappear without
retail exceptions. Caller casts make the canonical object/payload types explicit.
All 26 touched PC units compile, zero errors. Configure + ninja and separate
check_image print MATCH in all eight sections before committing.

The rider-time fixture's clang command now also searches include/, as requested
by the comparison worker. Both it and the integer/RPC fixture retain their
shared-header preparation fixes from 4443e879. Fresh full runtime suite exits
zero with DHD_VUTEXT enabled, including the 6,000 VU0 comparisons, sound RPC
widths, real Handle callers, callback/member ABIs, GS/VU1/VIF, controllers,
timers/SIF, audio and D3D11 WARP. Concurrent FP/runtime work remains unstaged.

Final fresh full clang audit: 3,797 units prepared, 1,196 warning flags and
162 permutations; the generated manifest equals port/abi_calls.json exactly.
Running round-4 flags: 1,634 -> 1,630 -> 1,261 -> 1,196 (438 removed).
79 distinct source units converted this round; all compile for native i386.
This worker removed six complete PATCHES entries/six text operations. The
actual HEAD text count is 39 (47 at round start, six ours and two independent
renderer removals). No permutation is removed in round 4; all sixteen adjacent
setter permutations remain necessary. Full image/ABI checks are unchanged.

## Round 4: private struct census and next review plan

Local scan: build-try/shared-round4/struct-census.json. It balances struct/class
bodies, groups their first named field offsets, counts distinct units separately
from merged-unit views, then inspects candidate families. Offset similarity
is a search aid, not proof of a common type: +0/+4/+8/+C alone appears in 310
units and mixes Handles, vectors, packets and other records. Padding/offset
names alone do not establish field types or object sizes.

Priority outside the established rider/animation headers:
- Particle/render objects: 42 candidate units, 59 private views with fields
  among +2600/+2690/+26B0/+2720/+27C0/+2824/+2830/+2850. The exact first-four
  prefix +27C0/+2824/+2830/+2850 repeats in five units (00225458, 002256F0,
  002268E0, 00226B90, 00227518). Start with those and the merged 00228BE8
  spawners; 0022CDC0/0022CFB8 have 29/24 guarded reorderings across 14/11
  participating units, respectively. These are the largest remaining manifest
  target groups. The spawner matrix +26B0 and copied/retained +2734/+2738
  pointers provide stronger layout evidence than the generic prefix scan.
- Instance/node base: 17 candidate units contain signed-short +B4/+B6 views.
  002182D0 in 00215B68 initializes the +40/+C0 matrices and tags and exposes
  the low-word resource link plus upper state bits. Establish that base before
  combining it with SceneNode; pointer/resource and packed-flag overlays must
  remain distinct until proved. The separate 40-unit 64-bit +440 flag family
  mostly belongs to the wider physics/object overlays, so it is not blindly
  folded into the node base.
- Camera attachment: nine candidate units reference the +A10 boundary/link
  (001AE358, 001AEB20, 001AEB88, 001B0C10, 001B0D38, 001B0E00, 001B0EA8,
  001E7160, 001E7440). 001B0C10/001B0EA8 show +A50/+A60 as three floats vs
  quadwords; verify the complete vector extent. The separate +C0 blend camera
  in 001B3858 needs its own prefix before combining camera families.
- GS builders: 0021EB30/0021F0A0 duplicate the 15-bit NLOOP GIF-tag view.
  00225B20's packet begins with a 64-bit word, then +8/+C words. That generic
  fingerprint also matches sound requests/other objects, which are excluded.
  002249E0 proves the fourth argument is a packet pointer; callers 00225458
  and 00226B90 disagree as pointer vs unsigned address. Follow this writer's
  call family rather than treating all four-word records as GS packets.
- Sound started this round with the complete request/emitter headers. Next
  00201BD0 has 48 callback/return flags; 00201E08 has 23. Preserve the proven
  64-bit callback-data slot and existing return-scheduling exceptions while
  sharing the true callback signatures.

Next prototype targets by current warning count after sound: 00201BD0 (48),
00236B28 (27), 001AC780 (26), then 00235798/00201E08/001DB9E0 (23 each).
Review the timer registration's callback/context fields rather than retaining
its current int/int parameter names. Stop round 4 here for review.

## Round 5: timer callback and pool API

Fresh baseline remains 1,196 ABI flags, 162 permutations and 39 text patches.
timer.h shares the 0x14-byte TimerNode and 0x1C-byte pool/sentinel layout
proven by 00236A30's allocation stride and the list bodies. Registration
00236B28/00236B98 returns TimerNode *, formerly void/int in callers; callback
and context fields replace the definitions' int/int parameters. The runner
uses an unsigned delay return, adds it to current time, and removes nodes on
zero. A nonnull context calls TimerCallback(void *); null uses a distinct
TimerCallbackNoArg(void) cast, retaining the retail no-argument path. The pool
constructor, active/free-list transfer, reset, release and runner share types.
The C++ release method retains its matching method form without a vtable.

Forty-seven units converted, including all registration/runner/constructor
callers and seven implementation units. 00208660 casts the true node-pointer
registration result back to its existing int wrapper result; this avoids
changing that wrapper's public ABI. 0019EE98's extra runner argument 3 is
unused by the actual one-argument definition and is removed; retail still
matches. Data-labelled callback thunks are explicitly cast at registration;
their separate forwarding bodies are not claimed to be reconstructed here.

Configure + ninja and separate check_image print all-eight-section MATCH.
47/47 touched native units compile, zero errors. Scoped audit removes 27
00236B28, four 00236B98 and four 00236C60 flags: 1,196 -> 1,161. No new
warnings, no patches removed, permutations 162 -> 162. Guarded sites in the
edited units are regenerated together with the source. Other workers have
staged files; an isolated index stages only owned files and shared hunks, then
replays only this commit into the real index, preserving their staged work.

## Round 5: sound completion callback and asynchronous API correction

Thirty-eight units share SoundCompletionCallback(int result, 64-bit data),
SoundCallbackData, and the 16-byte SoundCommandReturn. The actual receiver
002013F8 calls both arguments; 00208C50 stores result through the low 32-bit
address carried by data. 00202048's command return table and 002013F8's reader
now share callback/data fields at +0/+8. 00201BD0, 00201E08 and 00201D80
producers and their callers use the same callback/data declaration instead
of zero-argument callbacks, int callback slots and pointer/32-bit context slots.
00201D80's typed 64-bit context also fixes two setup-order words naturally.

Important correction to the earlier port patch assumption: 00201BD0 and
00202048 are void asynchronous APIs. No source caller consumes their return;
00202048 has a bare early return. The public [989snd EE implementation](https://github.com/Ziemas/989snd/blob/master/ee/989snd.c)
corroborates void named-play callback/SendIOPCommandNoWait interfaces and the
two-argument completion callback. A residual flush result in v0 does not
establish a return contract. The earlier two port entries that fabricated int
returns/return statements are removed completely (five text operations).
Canonical void declarations restore all retail schedules; no exception or
replacement return patch is required. No external code was copied.

Configure + ninja and separate check_image print MATCH in all eight sections.
38/38 native units compile using only the proposed clean patches blob. Scoped
ABI flags 1,161 -> 1,073, permutations 162 -> 162. Round-5 running totals:
123 flags removed, two complete patch entries/five text operations removed;
actual text count 39 -> 34. Three temporary integer-return candidates were
rejected by exact comparison before establishing the actual void contract.

## Round 5: particle records and spawner API

Twenty-seven units share particle.h, including the five exact +27C0/+2824/
+2830/+2850 views and both spawner definitions in 00228BE8. Particle has the
proven 0x2860 array stride, transform at +26B0, direction/matrix references
at +2734/+2738 and packet pointers at +2830. Quadword projections keep the
explicit EE copies. 0022CDC0/0022CFB8 return an int particle index; caller
void and pointer results are corrected. Their vector/matrix parameters and
integer/float ordering are shared with their definitions.

Forty-three of the 53 spawner permutations are retired. Ten calls retain
typed, documented retail scheduling aliases: canonicalizing them changes
retail instruction order. Four formerly hidden function-pointer casts in
001DF920 are replaced by a typed int-return alias. Trial pointer aliases and
Vector4 stack-scratch replacements changed bytes and were discarded; the
existing two-operation scratch alignment patch is still needed. No patch
entry is removed in this batch.

Configure + ninja and separate check_image print all-eight-section MATCH.
27/27 native units compile. Flags 1,073 -> 1,066; permutations 162 -> 119.
Round-5 totals so far: 130 flags and 43 permutations removed, two complete
patch entries/five text operations removed (39 -> 34 text operations).

## Round 5: scene-node instance prefix

Nineteen units use scene.h: the 17 +B4/+B6 census views, constructor and its
two allocator callers (some are the same units). The constructor and scene
iteration bodies in 00215B68 now use the same 0x100-byte SceneNode as rider
transforms. The prefix includes its resource-address/ID view, byte flags,
16-byte position/scale vectors, matrices at +40/+C0 and short indices +B4/+B6.
64-bit flag types and operands are explicit before shifts, keeping upper
flag bits in the i386 header. Resource-header bitfield interpretations remain
local projections; they are not falsely combined into the instance itself.

Configure + ninja and separate check_image print MATCH, 19/19 native units
compile. ABI flags 1,066 -> 1,066, permutations 119 -> 119, no patch removed.
This batch removes duplicate object layouts rather than checker warnings.

## Round 5: rider camera prefix and APIs

Twenty-one units share camera.h and its selected APIs, including the +A10
rider-link census views, related +C0 blend/transform prefix, angle output and
camera rig matrix/target prefix. The same object passes through 001B30B8 and
001B0C10; this call chain proves the +C0 and +A10 views belong together.
001E7160/001E7440 are unrelated scalar arrays and deliberately excluded.
+A50/+A60 are full 16-byte vectors as proved by 001B0EA8's quadword copies;
001B0C10 still zeroes only xyz. 001B3E98's output is also a full vector.
001AE358 uses the shared RiderComponent and RiderPhysics.velocity rather
than private +16D0/+4180 copies. 001B0D38 takes one camera pointer; the unused
second int supplied by 001B0E00 is removed with retail bytes unchanged.
001B30B8's int value is corrected to the RiderComponent pointer stored at +C0.

Configure + ninja and separate check_image print MATCH, 21/21 native units
compile. ABI flags 1,066 -> 1,065, permutations 119 -> 119, no patch removed.
Running round-5 totals: 131 flags and 43 permutations removed; five text
operations/two complete patch entries removed (task-owned text count 34).

## Round 5: GS/GIF/VIF packet builders and final validation

Twenty-nine units share gs_packet.h: all callers/definitions of 00224660,
002246A0 and 002249E0, plus the two display-packet implementations. 00224660
returns a GsPacket cursor, correcting int and unrelated private struct/float
pointer declarations. Its named fields identify NLOOP/EOP, NREG/format/PRIM/
PRE and the register list. 002246A0 uses a raw unsigned word-stream cursor;
caller record/byte views are explicit projections, rather than private API
prototypes. 002249E0's fourth argument is a chain-start packet pointer, not an
unsigned address. Its third argument stays unsigned: addr >> 3 with command
0x14 proves a VU microprogram byte address encoded into MSCAL, not a host
data pointer. A trial source-pointer declaration was discarded accordingly.

GifTag, the 23-quadword display packet (stride +170), and GS register bitfields
are shared with explicit 64-bit words. The two NLOOP views now use the same
GifTag. GsPacket intentionally keeps word alignment because some builder
cursors point inside packed VIF streams. Field positions and MSCAL meaning
were cross-checked with [ps2sdk GIF tags](https://github.com/ps2dev/ps2sdk/blob/master/common/include/gif_tags.h)
and [VIF codes](https://github.com/ps2dev/ps2sdk/blob/master/common/include/vif_codes.h).
No external code copied. No added ABI permutation or port patch.

Configure + ninja and separate check_image print all-eight-section MATCH;
29/29 packet units compile natively. The fresh full 3,797-unit ABI audit has
zero compilation errors and agrees exactly with the working manifest; the
final packet refresh preserves that agreement. Flags 1,065 and permutations
119 unchanged in this layout/API batch. The complete runtime suite exits 0,
including callback/thunk/ABI fixtures, rider-time, timer, sound, GS/GIF/VIF,
D3D WARP raster and shared-header virtual layout checks. The rider-time
fixture's round-4 include-path repair remains effective.

Final round-5 counts: 172 distinct source units converted (47 timer, 38 sound
callback, 27 particle, 19 scene, 21 camera and 29 GS, with overlaps). Flags
1,196 -> 1,161 -> 1,073 -> 1,066 -> 1,065: 131 removed. Permutations 162 -> 119:
43 removed; the ten spawner exceptions remain guarded. Two complete PATCHES
entries/five text operations removed this round. Task-owned text count
39 -> 34; the concurrent renderer's 00117A10 conversion adds one operation,
so actual HEAD count is 35. All 35 HEAD text operations apply exactly once.
Across rounds 4-5 the owned total is eight entries/eleven text operations
removed. Checks and guards are not weakened.

Next round: 001AC780 (26 flags), 00235798/001DB9E0 (23 each), and the scene
resource/registration family 0017D3A0 (22). Finish the allocator/camera views
still outside the shared prefixes. For permutations, rank 001B1698 (19),
00214A18 (11), 00202E38 (9), then 001B3170/001AE630 (eight each). Retire them
only when canonical caller prototypes preserve retail bytes. Stop for review.

## Round 6 baseline and priority: rider/NPC interaction distance

Fresh full baseline: 1,065 ABI flags, 119 permutations, 35 text operations.
The rider/physics path revealed a dropped float: 00113398's callers supply
four arguments including distance, but its definition retained only three;
its call to 00113588 also omitted that callee's float distance. On EE the
incoming f12 survived; on PC the callee read a missing argument. Both APIs
now share their actual prototypes in rider.h and explicitly forward distance.
Five units converted. RiderPhysics shares the owner, +7C gate, +404 action
state and 64-bit +440 flags; Rider shares the relevant control bits, NPC type
and +7A68 gate. All additions retain asserted offsets and existing strides.

The native execution fixture exercises both real functions: distance 5 vs 11
at the ordinary ten-unit threshold, 25 vs 41 at the alternate forty-unit
threshold, and the disabling gate. It also exposed a 16-byte vector output
alignment issue in 00113398's float scratch. Explicit aligned(16) fixes the
native store and retains retail bytes. The fixture is wired into the runtime
suite, with only this worker's hook staged separately from another worker's
scalar-ABI hook. This repairs a rider/NPC control path; it is not evidence
that the observed race reset or bad bone matrix is fully fixed.

Configure + ninja and separate check_image: eight-section MATCH; 5/5 touched
PC units compile and the interaction execution fixture passes. Flags
1,065 -> 1,059 (five dispatcher arities plus its missing callee distance),
permutations 119 unchanged, no patch removed.

## Round 6 priority: NPC animation handles and selector slots

Three NPC animation/movement units (00124B40, 00126238, 00126B20) now call
shared pointer APIs instead of passing a 16-byte packed vector/name/Sound
aggregate to 00203E10/00203DC8/00203B68. The objects are AnimationHandles,
not names or positions. Existing stack copies retain the EABI hidden-pointer
copy schedule; passing the original record directly is not byte-identical.
00126B20 also corrects its selector call into 00202E38: on PC a 64-bit integer
formal shifted mode/flag stack slots. An explicit native inline bridge casts
the retained long selector into the shared pointer API. The MIPS-only DI
alias is kept because replacing it changes the matching register schedule.
This exception is explicit; no checker or matching rule is weakened.

The native execution fixture runs the real NPC state-change function for
three model kinds. Shared-prototype stubs verify handle contents, seek/apply
modes, selector slots, durations, random progress and speed/rate values.
The old by-value declarations or 64-bit selector call would violate these
checks. 00126B20's one-operation aggregate/reference patch retires together
with the source change; all its native calls use the actual shared pointer
APIs. No new permutation is needed.

Configure + ninja and separate check_image: eight-section MATCH; all 3 native
units compile. Flags 1,059 -> 1,051, permutations 119 unchanged. One complete
patch entry/one text operation removed; round-6 running totals are 14 flags
removed and one patch operation removed. This is a concrete NPC animation
rate/control repair, not yet independent evidence of correct rendered bones.

## Round 6: shared animation dispatch controller

Fourteen units use the 0x64-byte AnimationController in animation.h.
001AC780 returns its controller pointer, correcting the void/int declarations
in initialization callers, with its byte/short argument widths shared too.
The dispatch/fade implementations 001AC680/810/880/920 share the same handle
pointer and +44/+5C float fields. The constructor's C++ member-shaped call in
00127CC8 is now an ordinary shared C call on the embedded controller, keeping
the loader's other offsets/total size. The six-element controller array in
00164C38 uses the shared stride rather than an opaque 0x64 placeholder.
The three explicitly excluded CRLF-only files remain untouched.

Configure + ninja and separate check_image print MATCH; 14/14 native units
compile. Flags 1,051 -> 1,025, permutations 119 unchanged. No patch removed.
Round-6 running totals: 40 flags and one text operation/entry removed.

## Round 6: object registry and collision/dispatch lookups

Forty-two units share object_registry.h. 0017D3A0 stores an object pointer
and returns an integer slot; 0017D3D0 returns the stored pointer, not an int.
The table and initialization/lookup/registration bodies share pointer storage.
Collision/contact/dispatch callers use explicit views of that returned object.
The implementation consumes only the first registration argument. Twenty-one
old calls also set an unused resource argument (and some declare short
returns); removing that setup changes retail bytes in every trial. Shared
MIPS-only scheduling aliases preserve those calls. Native inline bridges
invoke the one-argument canonical pointer API, retaining explicit short
narrowing where the old caller used it. They do not invent a second consumed
resource or hide a new ABI permutation. All API/bridge declarations are in
one header; the exact-match and ABI checkers are unchanged.

Configure + ninja and separate check_image print MATCH; 42/42 native units
compile. Flags 1,025 -> 1,003, permutations 119 unchanged. No patch removed.
Round-6 running totals: 62 flags and one text operation/entry removed.

## Round 6: SKAT queue stop/update and four-team membership API

Twenty-six units share skat_queue.h/team_roster.h. 00235798 returns an int
status, correcting 23 void-return call sites. Its stop/update partner shares
the 0x3928 queue stride, state, ring indices, pending-callback byte and sound
handle. +3918 carries a 32-bit sound handle passed into the command-47 RPC,
not a Voice object pointer. The full-word unsigned sentinel comparison retains
the retail schedule; a signed temporary shortened the function and was rejected.
001DB9E0 returns whether any of four teams has the requested ID, correcting
23 void-return call sites. It shares the 0x30 item, 0x240 team and membership
roster layout and explicitly declares a four-word output buffer. Existing
caller buffers already hold all four words; no truncation/overflow is hidden.

Configure + ninja and separate check_image print MATCH; 26/26 native units
compile. Flags 1,003 -> 957, permutations 119 unchanged. No patch removed.
Round-6 running totals: 108 flags and one text operation/entry removed.

## Round 6 priority: rider input/camera channel float forwarding

Thirteen units share CameraParameter (0x50 stride) and the thirteen channels
at RiderCamera +120. 001B3028 takes index and float value and forwards both
to 001B2E50; its old definition/prototype dropped value and relied on retained
f12. Two 001ACFA8 rider-controller calls had float/index order reversed;
canonical order matches all retail bytes. 001B3068 enables the shared array
instead of an opaque private Item layout. The native fixture executes the
three real functions, checks every channel, a negative value, a disabled
channel and the zero-value gate. It passes. The existing three-operation
forwarding patch retires with the source; two permutations also retire.

Configure + ninja and separate check_image print MATCH; 13/13 native units
compile. Flags 957 unchanged (the removed port patch already repaired its
native audit shape), permutations 119 -> 117. Round-6 totals so far: 108 flags,
two permutations and two complete patch entries/four text operations removed.
The rider input path is now explicit in shared source; race/bone behaviour
still requires independent port validation.

## Round 6 priority: NPC float-return stack leak

Six NPC movement/proximity units share npc_motion.h and the actor/node/world
prefix. 0015A968 returns a float distance; its retail epilogue explicitly
reloads f0 from the distance stack slot. Five callers instead declared void.
On the i386 native ABI this matters even when the result is discarded: the
callee pushes an x87 return and a void caller never pops it. The execution
fixture runs the actual 0015AB08 caller and 0015A968 callee sixteen times.
With the shared float declaration it passes and status is 0x0000. Restoring
the wrong void declaration via --wrong-prototype fails with x87 invalid/
stack-fault bits 0x0041 (observed full status 0x0241). This is a demonstrated NPC FP-stack corruption
mechanism; a link to the live bone/race failure still needs port tracing.
The fixture is part of the runtime suite. No matching check is weakened.

Configure + ninja and separate check_image print MATCH; 6/6 native units
compile. Flags 957 -> 952, permutations 117 unchanged. No patch removed.
Round-6 running totals: 113 flags, two permutations and two complete patch
entries/four text operations removed.

## Round 6: collision-query API, permutation trials and final validation

Twelve collision-query units now obtain 00214A18's Vector4/SpatialHit/count/
float prototype from spatial.h. Matrix helper projections in 0019D588 and
namespace-local declarations in 00148F60 use the existing vector.h API;
this avoids conflicting native C declarations after namespace conversion.
The caller-private hit field views are still present, and are not counted
as completely reconstructed shared hit layouts in this batch.

Canonicalization was tried on the 35 highest-ranked remaining rider blend
permutations: two calls in 00162FB0 match canonically, 33 still change retail
instructions. Next, eleven 00214A18 collision-query permutations were tried:
two match canonically and nine still need typed scheduling aliases/guards.
The two input-channel permutations retired earlier make six removed this
round. No matching or stale-site check was relaxed to obtain that count.

Final round-6 scope: 120 distinct source units, with additional overlap among
five interaction, three NPC-handle, fourteen controller, forty-two registry,
twenty-six queue/roster, thirteen input, six NPC-float and thirteen final
collision/blend-batch units. Flags 1,065 -> 1,059 -> 1,051 -> 1,025 -> 1,003
-> 957 -> 952 (113 removed); no symbol gains warning flags. Permutations
119 -> 117 -> 115 -> 113 (six removed). Two complete patch entries/four text
operations retire this round. The independent port-worker scalar/particle/
blend repairs add five text operations; actual HEAD text count is 36, compared
with the 35 baseline. All 36 HEAD text operations apply exactly once.

Full configure + ninja and separate check_image print all-eight-section MATCH
before each commit. Final 120/120 touched units compile natively. A fresh full
3,797-unit ABI audit compiles without errors and its manifest equals the
working manifest exactly. The complete runtime suite exits 0, including the
four new real-path fixtures, existing rider-time/handle/speedometer tests,
all semantic/VU/GS/VIF/controller/timer/sound checks and WARP raster tests.
The integer-ABI fixture now permits retired patch entries, expands the new
headers before independent symbol renaming, and models the true NPC float
return; its assertions remain active. The restored wrong NPC return prototype
fails the negative fixture with x87 stack-fault/invalid bits set.

Priority handoff: the demonstrated NPC x87 leak, NPC handle/selector slot
corruption, dropped interaction distance and implicit input-channel float
forwarding should be checked against the port worker's first bad bone/race
state. This round does not claim the live race-reset or bone rendering problem
is solved. Audit the remaining float/void pairs first: 001DDA50 (two callers),
00121348, 0012AD80, 0015B910 and 002362E8; 001797A0 also consumes bogus float
returns from void 0023D650/0017B848 and needs a careful matching reconstruction.
Then return to 00192BA8 (20 flags), 0020DC28/0018D9D0 (19 each) and further
permutations. Stop round 6 here for review.


## Round 7: path-track float return

001DDA50 returns the segment dot product through f0 (retail mov.s f0,f20
at 001DDB30); both 0016FBB0 and 00171DD0 falsely declared void. Four units
now share path_motion.h, PathTrack and its node/vector prefix. Both discarded
results now receive native x87 cleanup. Full configure/ninja/check_image MATCH;
4/4 native units compile. ABI flags 952 -> 950, permutations 113 unchanged.
No patch removed. The full floating-return inventory/guard follows this batch.


## Round 7: complete floating-return inventory and fixes

The baseline has 43 declaration/definition return disagreements across 29
callees: 39 wrong declarations of proven float/double callees, three false
float declarations of void callees, and the fabricated float definition of
001797A0 (whose void caller was already correct). The inventory counts one
edge per source declaration; repeated calls and callback stores are not counted
again. The old AST audit missed assembly-only functions and address-taken
handlers, so its return-warning count was not a complete inventory.

All 43 edges are corrected, with zero remaining floating-return guard errors.
The original transformed units were reconstructed from this round's source
backups and the baseline committed patches; every changed unit was inspected
again using the final guard. The full 3,797-unit current inventory checks all
function declarations (including headers), GNU89 implicit calls, direct/asm
aliases, callable casts, address-taken functions and cross-unit callback tables.
The initial inventory also records 51 bad function-to-pointer conversions/call
references; these overlap the declaration edges and are not an additive count.
No additional FP disagreement appears when implicit declarations and indirect
function-pointer casts are included.

Return-only inventory (source filenames identify every baseline edge):

| Callee | Baseline vs canonical return | Units |
|---|---|---|
| 00121348 | void -> float | 00120838.c |
| 0012AD80 | void -> float | 0012A9B8.c |
| 00156568 | float -> void | 0010B0E8.c |
| 0015B910 | void -> float | 0015B100.c |
| 001797A0 | definition float -> void; caller void is correct | 00176ED8.c |
| 0017B848 | float -> void | 001797A0.c |
| 001DDA50 | void -> float | 0016FBB0.c, 00171DD0.c |
| 001FC810 | int -> double | 001BEE28.cpp |
| 001FCDF0 | int -> double | 001BEE28.cpp |
| 001FD300 | int -> double | 00127F20.c, 00177DA0.c, 00178730.c, 00199338.c, 001A2EF8.c, 001BEE28.cpp, 0022FC98.c |
| 00206E40 | void -> float | 00207630.c |
| 00236260 | void -> float | 002362E8.c |
| 002362E8 | void -> float | 00123AD0.cpp, 00124B40.cpp |
| 0023D650 | float -> void | 001797A0.c |
| 00248A80 | int -> double | 0022FC98.c |
| 00260E08 | void -> float | 0012A3E8.c, 0012A4D0.c, 0012A570.c, 0012A6D8.c, 0015AD60.c, 0015AEE8.c, 00180A10.c |
| 002627A0 | void -> float | 00207630.c |
| 002628A0 | void -> float | 00207630.c |
| 00262B48 | void -> float | 00207630.c |
| 00262C20 | void -> float | 00207630.c |
| 00262D30 | void -> float | 00207630.c |
| 00262FE0 | void -> float | 00207630.c |
| 00263270 | void -> float | 00207630.c |
| 00263320 | void -> float | 00207630.c |
| 00263438 | void -> float | 00207630.c |
| 00263578 | void -> float | 00207630.c |
| 00263860 | void -> float | 00207630.c |
| 002638D8 | void -> float | 00207630.c |
| 002639D0 | void -> float | 00207630.c |

The true ground-distance probes 00121348/0012AD80/0015B910, vector-distance
helpers 00236260/002362E8 and assembly projection 00260E08 now use
rider_geometry.h. The three ground actor prefixes and marker/speed objects
have shared opaque identities; their per-unit field views remain private.
002362E8's output is a Vector4 pointer, replacing the false int formal.
All seven void 00260E08 callers now pop the native x87 result. The fourteen
animation table entries use AnimationHandler's float return and time argument,
including 00206E40's getter with unused int/time slots. The table initializer
and dispatcher now agree on the complete callback type.

001797A0's m2c return chain was artificial: the size getter 0023D650 and packet
writer 0017B848 are void; retail does not prepare an f0 return at the marker
loop's exits. Removing both invented assignments and the invented float return
matches exactly. 00156568 is likewise a void speed-scale setter, not a float
getter. These corrections avoid reading/popping nonexistent x87 results.

soft_double.h fixes 001FD300(float)->double, 001FC810(double,double)->double,
001FCDF0(int)->double, 001FD010(double)->float, 001FCDA0(double,double)->int and
00248A80(double)->double. The HUD/debug formatting and actor-distance locals
now retain the full double. On this EE toolchain a software double travels as
64 bits in GPRs, not f0; on native i386 the result is in x87. All eight touched
units match with canonical double prototypes, so no port return patch is needed.
The existing speedometer host-varargs patch remains for its distinct EE-format
wrapper boundary.

## Round 7: permanent guard and recompiled-assembly boundary

abi_check.py now fails before writing/updating the manifest if any proven FP
callee has a non-FP/different-precision declaration or pointer conversion.
It also rejects conflicting FP definitions/table declarations, incompatible
casts of indirect float function pointers and false FP declarations of integer/
void definitions. GNU89 implicit functions are extracted from referencedDecl
when clang does not emit a standalone declaration. Included header declarations
are checked even though their offsets cannot be mapped into a source call patch.
Static C++ overloads retain their mangled identities; this removes six old false
Normalize warnings. The improved implicit-call handling exposes one additional
non-FP mismatch (00187438 -> 00190188), retained in the report.

port/float_returns.json records 48 reviewed assembly-only FP contracts, including
SDK float math, projection primitives, thirteen animation handlers and software-
double helpers. Retail f0 scratch writes are not classified as returns: the
world-extrema loop 0011A438, output-vector routines 00151F38/001520E0, and integer
conversion/boolean routines illustrate why. 00159778 is an epilogue remnant,
not a callable getter. C definitions provide the other return contracts. No
retail assembly or game data is committed; all evidence comes from the existing
local disassembly/definitions, without third-party code.

Recompiler cache version 3 invalidates stale inferred signatures. Projection/
software-double overrides apply on every invocation, and every reviewed assembly
return overrides its inferred cached result. This repairs native wrappers and
native-call result marshalling, including FPR(0) for float and the full GPR(2)
double payload. Existing forwarding-thunk inheritance remains active.

The permanent runtime guard fixture rejects direct void/int returns, double
loss, undeclared functions, wrong header declarations, asm-only callees, direct
and indirect callable casts and callback-table type disagreements. Correcting
all types clears the same guard. Stale projection/double wrapper caches and
marshalling are checked as well. Existing 845 animation-callback cases and
forwarding-thunk execution tests remain active.

## Round 7: high-count model API and final validation

Eight model units now share model.h: 00192BA8 takes one resource ID and returns
its category. Its former second formal was overwritten before every use; making
it a local matches and removes twenty arity flags. 002623C0 returns the class word
bits 18..31 through the actor +4 chain; pointer-return guesses become int, all
callers share the explicit object prototype, and the former C++ method-style
call matches as the canonical API. 001933A0's explicit this forwarding matches,
retiring one complete patch entry/two text operations. Its real-body member ABI
fixture permits the retired entry and expands model.h before native lowering;
all original vtable/this/result assertions remain active.

Round-7 scope is 53 distinct units. Return disagreements 43 -> 0; ABI warning
flags 952 -> 914 (33 actual return/arity sites removed, six checker-overload
false positives removed, one newly exposed implicit non-FP mismatch). ABI
permutations 113 -> 113. One patch entry/two text operations removed. Full
configure + ninja and separate eight-section check_image print MATCH before
each commit. All 53 native units compile. The final full 3,797-unit audit
compiles, reports no floating-return guard failures and exactly matches the
committed permutation manifest. The entire runtime suite passes, including
real NPC x87-balance, rider/input/handle/speedometer, callback/thunk, GS/VIF/VU,
controller/timer/audio and raster tests.

Validation uses committed patches plus this round's owned removal; the port
worker's uncommitted x87 observer patches are excluded from the runtime fixture
snapshot because their regex instrumentation needs separate fixture support.
No observer/renderer/audio diagnostics are staged. The stock in-progress suite
initially stopped in that worker's rider-time patch setup; the owned snapshot
passes. The guard's own negative/positive regressions also pass independently.

Next round: 0020DC28 / 0018D9D0 (19 flags each), then 002238A8 and 00231CC0,
with live rider/bone traces directing any priority changes. Floating-return
contracts are now a hard checker failure. Stop for review.

## Round 8: scalar arguments, wide values and explicit native contracts

Scope: 191 matching source units; five additional native-only units repaired
through patches (00231CC0 / 00231D88 and the three protected CRLF-only
001AC620 / 001AC7B0 / 001ACA08 units). No protected source change is staged.
New headers: disc_path.h, menu.h, format.h, impulse.h, scalar_api.h,
sound_math.h, scene_effects.h and particle_update.h.

The priority rider/camera/particle disagreements are:

- 001B3D58: `(Obj *, int, int, float)` -> `(Obj *, int, float)`;
  the extra integer hid delta on i386. Six camera channel updates now call
  the canonical API; the real caller and setter execute in the fixture.
- 001C2458: six pointers, a false DI flag, two floats, a short material mask
  and flags -> six pointers, SI flag, SI mask, two floats and flags. The
  material mask must precede the float slots. Both rider trail paths now
  forward the actual EE integer and FPR inputs; the fixture verifies all
  eleven arguments and truncates only the proven SI flag.
- 0022B028: six GPR arguments -> six GPR arguments plus float velocityScale (named deltaTime in round 8; corrected below).
  Its handwritten body reads incoming f12; 0022B1F0's former `unused`
  parameter supplies it. The matching caller keeps its retail setup while
  native C explicitly forwards it; t0 is the vector offset pointer. The real caller fixture checks the multiplier,
  four value lanes, addresses, count and offset.
- 00208C50 / 00235E18 / 00235E70: erased `void(void)` callback addresses ->
  `void(int, uint64)` completion callbacks, including weather sound aliases.
  Seventeen address/callback disagreements are included in the DI count.
- 001F97A8 / 0017ED50 return only zero/one: false native DI returns -> SI
  predicates. Keeping a DI spelling only for the EE preserves its scheduling.
- 0023B220 / 0023D4F8: SI guesses -> three DI tag words / four DI RGBA
  components. Actual DMA-tag and GS packet writers execute with high-word
  assertions in the fixture; subsequent slots no longer shift on i386.

Other scalar APIs in scalar_api.h retain uniquely named opaque object tags;
this round does not claim their private field layouts have all been merged.
The adapters spell out the consumed EE register-bank arguments. Several
retail calls cannot adopt the canonical declaration/order without changing
instruction scheduling; the MIPS compatibility branches/asm aliases preserve
those instructions and native calls use the one shared canonical prototype.
There is no runtime signature guess in these adapters.

0020DC28 has four string arguments; the historical fifth device-mode setup is
ignored by the callee. 0018D9D0 now owns the complete 0x4FC SharedMenu constructor
layout, its 16-byte items and 8-byte links. Six actual C++ base constructors
retain their matching class declarations. 002238A8 returns the written character
count. 00231CC0 / 00231D88 really return void: their fake int return chains are
removed only in native patches because retail scheduling requires the old
source form. ImpulseBody owns the matching position, moment, inverse-inertia
and velocity-delta layout. 00208C60 and 0020ECD0 share their canonical APIs;
explicit compatibility adapters replace eleven permutation guesses.

### Complete inventories and permanent checks

Baseline native calls were reconstructed from accepted round-7 source and
patches, after applying its 113 safe permutations. The bridge baseline uses
HEAD's version-4 recompiler, native baseline definitions and its original
return catalog; this avoids counting the audio worker's already-fixed DI/TI
aliases or round-7 software-double overrides again. Counts distinguish C
reference sites, inferred bridge contracts and shared marshalling paths:

| Audit category | Before | Remaining |
| --- | ---: | ---: |
| Native float/double arguments | 39 | 0 |
| Recompiled float/double argument contracts | 24 | 0 |
| Formatted vararg paths losing promoted doubles | 2 | 0 |
| Native DI arguments/returns and callback references | 39 | 0 |
| Recompiled DI contract (001FCFE0 mantissa) | 1 | 0 |
| Formatted vararg paths losing DI values | 2 | 0 |
| Additional fixed-signature TI disagreements | 0 | 0 |
| Formatted vararg paths losing TI values | 2 | 0 |
| Additional assembly FP forwarding return contracts | 7 | 0 |

Thus FP argument boundary repairs total 65; DI repairs total 42. The TI sweep
confirms all 13 public scalar-TI APIs (arguments and returns) agree. The audio
worker's quad_abi.h / version-4 mechanism already repairs the two stale sound
name cache guesses: those are not counted as new TI declaration fixes. The
new format-vararg reader also preserves all four TI words and following args.
Scalar TI remains distinct from a 16-byte aggregate passed by hidden pointer.

scalar_abi.py is a hard failure in abi_check.py. Its second pass audits effective
native code after the candidate safe permutations; a failed transform cannot
reuse an old AST/source or publish a manifest. It covers direct, implicit/default-
promoted and constructor calls, callback escapes, callable casts, and conflicting
exported callback table declarations. native_scalar_contracts.json records 373
proven native contracts, checked against definitions on every audit; small signed/
unsigned byte returns keep their actual C return convention. scalar_contracts.json
records 73 reviewed asm/intrinsic/variadic contracts. An additional asm input
census rejects unreviewed incoming FPR or quadword arguments; it is a review
trigger, never justification to invent a prototype. Wide arguments to unknown
asm variadic bridges also fail. Negative/positive fixtures exercise each guard.

Recompiler version 5 overrides even a current declaration cache with these
contracts. Every known scalar callback reached by jalr uses the same typed
GPR/FPR/stack marshalling as direct calls; integer-only/external targets retain
the existing fallback. The matching 00222170 projection builder proves float
arguments past f19 occupy eight-byte stack slots, now supported and executed in
the regression. Synthetic jalr cases check float/double/DI/TI results, full
payloads, later argument positions, void results and fallback behavior.

The two formatted wrappers (00254D08 printf / 00254D68 sprintf) previously read
every native vararg as uint32. A format-aware reader now transfers promoted
doubles, DI values, TI values, pointer/word args and `*` fields into EE slots.
It honors native clang i386 TI padding and aligned 16-byte EE overflow slots.
Both generated wrappers execute with mixed arguments and register/stack spills;
the guard rejects a new wide asm variadic call without a reviewed bridge.

The seven additional FP return contracts are the pack/unpack forwarding
helpers __pack_f / __pack_d, 001FC7B8, 001FC878, 001FCB20, 001FCFE0 and 001FD2D0.
Their complete software-double or f0 results were absent from the original
catalog; no native floating-return mismatch is reintroduced. Helper meanings
were checked against the primary GCC fp-bit.c source:
https://github.com/gcc-mirror/gcc/blob/master/libgcc/fp-bit.c
No third-party implementation was copied. Prototype widths/register inputs
were established from the local retail disassembly and matching definitions.
No proprietary asm, ELF, assets or data is committed.

### Round 8 counters and validation

ABI warnings: **914 -> 757**. Permutations: **113 -> 102**. Patches removed this
round: **0 entries / 0 text operations**. Added: **5 entries / 15 operations**
for the explicit retail scheduling exceptions. At commit preparation HEAD has
33 patch entries / 39 text operations after another worker's independent rider
fix; our resulting tree has 38 / 54. All 54 text operations apply exactly once
in application order. Only the five owned patch entries are staged.

Full configure + ninja followed by the independent eight-section check_image
prints MATCH. All 196 affected native units compile. The complete 3,797-unit
ABI audit compiles twice, matches the 102-site manifest and reports zero FP
return, FP argument, DI or TI failures. The runtime suite and scalar/format/
quad/callback fixtures pass; the port links with its existing 23 SDK stubs and
no unresolved symbols/duplicates. Shared-file commits contain only owned hunks;
renderer/audio/comparison diagnostics and CRLF-only source changes stay unstaged.

Next: the remaining 19-call 001B1698 blend permutation family, then 001F7640 /
001F7680 menu APIs and the other highest-count flags; continue with explicit
matching compatibility adapters where source scheduling forbids the canonical
order. The scalar guards stay mandatory. Stop for review.


## Round 9: rider blend scheduling adapters

Fresh full baseline: 757 flags and 102 permutations. Five rider/NPC caller
units move the 001B1698 scheduling declarations into tween.h. The canonical
API remains int(controller pointer, int frames, float weight); weight-first
and discarded-return MIPS aliases preserve retail setup/scheduling. Native
inline adapters reorder explicitly and call that single prototype, removing
19 guarded port permutations and three false void-return flags. A real setter
fixture verifies frames, initial/target weights and returned tween status for
all three adapter forms. This change replaces existing port repairs; it does
not establish a new live race/bone cause.

Configure + ninja and separate check_image: eight-section MATCH. All five
native caller units compile; full scalar/return guards pass with the refreshed
manifest. Running flags 757 -> 754; permutations 102 -> 83. No patch removed.


## Round 9: menu select/back sound API

001EB1D8.cpp's fifteen select and fourteen back sound calls now share menu.h.
The actual implementations only submit S_SELECT/S_BACK to sound; neither
reads a menu pointer. They have real names PlayMenuSelect/PlayMenuBack and
canonical void(void) prototypes. Shared MIPS-only one-pointer aliases retain
the retail caller setup; native adapters invoke the zero-argument API. All
29 private declarations disappear, without conflating the unrelated menu
object layouts with SharedMenu. No port patch/permutation is added or removed.

Configure + ninja and separate check_image: eight-section MATCH. The complete
merged unit compiles natively; scoped full-graph audit flags 754 -> 725,
permutations 83 unchanged. Commit this API independently for bisection.


## Round 9: resource-name getter

Fourteen units share 001BA3A8 through disc_path.h: it returns the global
char pointer D_004BD4DC, and reads no argument. Thirteen callers previously
passed D_004B25A0, eleven also declared/stored the result as int. Shared
GetResourceNameLegacy preserves the ignored EE setup while native calls use
char *(void); the resource/path locals now retain pointers. All fourteen
units compile natively and configure + ninja + separate check_image print
MATCH. Flags 725 -> 712; permutations 83 unchanged; no patch removed.


## Round 9: secondary/body rider blend APIs

Nine units move the remaining 001B3170/001AE630 weight-first declarations
into tween.h. The shared secondary/body prefixes and canonical pointer/int/
float APIs already existed; explicit native adapters now replace all fourteen
remaining argument permutations, including 00181CB0's float-first form.
MIPS aliases retain the exact setup. The real three-setter fixture checks
frames/weights and status across every adapter form. Full image MATCH and
9/9 native compiles; flags 712 unchanged, permutations 83 -> 69; no patch
removed. These replace prior guarded native repairs, not a new gameplay fix.


## Round 9: animation selector API and menu resource width

Nine units share animation_select.h, included by animation.h. 00202E38's
canonical API is void(handle pointer, int mode, int flag, float duration);
nine time-first permutations now use a named shared native adapter and MIPS
scheduling alias. The existing NPC DI-address bridge also moves into this
header. 001A5218 and the merged menu unit remove private selector declarations
and project their handle views explicitly. No rate/slot semantics change.

The menu unit's 00203590 selectors share void(handle pointer, int resource,
int mode, int index). Its old main declaration used a DI long resource; the
shared MIPS scheduling alias retains that shape while native forwarding takes
a 32-bit resource. All its local 00203590 aliases now use the canonical API.
The corresponding width-rewrite text operation retires from patches.py in the
same commit; its separate movie bypass remains. No patch entry is removed.

Configure + ninja and separate check_image: eight-section MATCH; 9/9 native
units compile. The real NPC fixture and shared selector/three-blend-setter
fixtures pass. Full 3,797-unit strict float/DI/TI audit passes with zero scalar
errors. Flags 712 unchanged; permutations 69 -> 60. One text operation removed.


## Round 9: UI style record and initializer/apply APIs

Twenty-seven units share ui_style.h and its 0x20-byte UiStyle. 0018D7D0 returns
that pointer, rather than void/int/another private record type; 0018D800 takes
one const style pointer and returns void, fixing four false int-return calls.
The initializer/apply bodies, local Rect and opaque Widget/Rec style copies
now share the same record. Other raw global/panel views are explicit projections.
The renderer's 0020FB80 stores x/y and x+width/y+height; 0020F518 stores RGBA,
proving the field names without guessing from constructor constants. Every
initializer writes all eight words; the stack Rect is the full 32 bytes.
MIPS discarded-return aliases preserve the twelve former void call schedules.

The real initializer/apply fixture verifies the returned pointer, all eight
values and adjacent canaries, including negative width/height and the native
discard adapter. Configure + ninja and separate check_image: eight-section
MATCH; 27/27 native units compile. Flags 712 -> 696; permutations 60 unchanged;
no patch removed. This is one UI API family, committed independently.


## Round 9: particle caller trace and corrected scalar meaning

Correction to round 8's deltaTime name: 0022B028's missing f12 float is a
**velocity decay multiplier**, now velocityScale. It is not elapsed time.
0022DE40 obtains the integer frame increment from 00238F60 and passes it to
0022B708. The assembly caller converts that integer to f20, constructs the
per-particle update values and calls 0022B1F0 at 0022B8AC: f12 is loaded from
sp+0x20 (derived from 1 - object[0x2778]), f13 from sp+0x24 (displacement scale),
f14 from f20 (age increment). B1F0 forwards f12 to B028 and builds values as
{f13, f13, f13, f14}. B028 adds offset + position + velocity * values, then
multiplies velocity.xyz by f12 and compacts expired particles via position.w.
This proves a real particle-motion ABI repair, without establishing a direct
cause for the race clock/reset or zero MPH. The frame getter is shared with
rider time paths, but no new disagreement in that getter was found.

Only parameter/comment names change in this batch: existing forwarded bits
and signatures remain intact. Full image MATCH and the native real B1F0 caller
fixture pass; the 00228BE8 unit compiles natively. No new flag, permutation or
patch change. These conclusions use the local retail disassembly and matching
C only; no external implementation was copied.

### Port-worker checklist: rider/race/physics/input/animation fixes since round 5

| Round / commit | Fix to exercise in the port |
| --- | --- |
| 5 / 16e2b084 | camera.h: camera +C0/+A10 rider links and full +A50/+A60/output vectors; 001AE358 reads shared rider velocity. |
| 6 / 8d1d30fc | 00113398 -> 00113588 explicitly forwards float interaction distance; aligned 16-byte output scratch. test_rider_interaction.py. |
| 6 / 72edee70, 515b6138 | NPC animation handles passed by pointer; 00202E38 selector slots retained; shared 0x64 controller/constructor return. test_npc_animation.py. |
| 6 / 327b4349 | 001ACFA8 -> 001B3028 -> 001B2E50 forwards float input and correct index/order into 13 camera channels. test_rider_input.py. |
| 6 / f4e4231a | 0015A968 float distance return replaces void NPC callers: demonstrated x87 stack leak after repeated calls. test_npc_float_return.py. |
| 7 / 7bce00a0 | ground/vector/projection distance float returns, software-double HUD conversions, animation-table float returns/time args; false speed-setter float removed. test_float_abi.py. |
| 8 / a2d562d2 | 001B3D58 camera delta loses the extra int; 001C2458 rider trail flag/mask/float slots corrected; ImpulseBody and native void impulse return chains. test_shared_scalars.py. |
| 8-9 / a2d562d2, particle note above | 0022B1F0 -> 0022B028 explicitly forwards velocityScale; particle displacement/aging inputs are separate. test_shared_scalars.py. |
| 9 / d9a7d7f3, 34068996, c482dfb3 | blend/selector scheduling adapters replace 42 existing port permutations; menu animation resource slots use a shared SI prototype. test_shared_scalars.py and test_npc_animation.py. |

The executable fixtures establish the call contracts. Live bone matrices,
rider speed and race-reset behaviour still need the port comparison run.


### Round 9 final counters and validation

Sixty distinct source units converted, in separate API-family commits: main
rider blend (5), menu sounds (1 merged unit), resource name (14), secondary/
body blends (9), animation selection (9), UI style (27) and particle naming
(1), with six overlaps. Flags 757 -> 754 -> 725 -> 712 -> 696: 61 removed.
Permutations 102 -> 83 -> 69 -> 60: 42 removed. One text operation removed,
zero complete patch entries removed; actual patches.py has 38 entries/53 text
operations, and every HEAD text operation applies exactly once in order.

All 60 touched units compile natively; the final full 3,797-unit check has
19,280 direct calls compared, agrees with the manifest and has zero FP-return,
FP-argument, DI or TI guard errors. Configure + ninja and separate check_image
print eight-section MATCH before every commit. The complete runtime suite exits
0, including real rider/NPC, new shared blend/selector/UI-style and particle
fixtures. The standalone integer-ABI fixture now expands animation_select.h
before its symbol-prefix rewrite; that transitive include failure was corrected
in its own small follow-up commit. No matching/ABI guard was weakened.

Next: 001848E0 and 001873C0 (10 flags each), then 00214BA0/00119180/00230F60/
002428E0 (8 each). Remaining permutations rank 00214A18 (9; another worker
currently touches its unit), 00211980 (6), 0022CDC0/0022CFB8/00203B68 (5 each).
Coordinate the touched collision unit; keep prioritizing proven rider/animation
contracts, without inventing prototypes for unresolved callees. Stop for review.


## Round 10: menu selection and item count APIs

Baseline 696 flags / 60 permutations. Sixty units share menu_items.h for
001848E0, 001873C0 and 0018E3A8. Selection consumes only (state, index),
retaining the old third setup slot through a shared MIPS alias/native adapter.
The item wrapper returns the updated count, correcting ten void-call flags.
Its underlying item append API shares the SharedMenu/16-byte MenuItem layout
and stores a context address in the existing word field. Private menu views
are explicit projections; this does not merge unrelated derived class tails.
The real three-function fixture checks selection, counts, IDs/context/flags.

Configure + ninja and separate check_image: eight-section MATCH. 60/60 native
units compile and the fixture passes. Flags 696 -> 676; permutations 60
unchanged, no patch change. These are declaration repairs; no claim about
live race/physics behavior is inferred from menu type names.


## Round 10: reviewed false positives with proof retained

Of the post-menu 676 flags, 81 are false positives: 76 GNU89 no-prototype
calls supply exactly the callee's SI-only arguments after default promotions,
and their return ABI already agrees; five 00161868 calls pass packed Color.
The Color union is a four-byte/four-aligned POD. Native LLVM lowering confirms
one four-byte stack argument (byval for C++ linkage, i32 for C linkage), matching
the caller's packed word. Size/alignment/POD and current lowering are rechecked
against the transformed definition; the result cache is keyed by source hash.
An eight-byte Color fixture fails the equivalence check and stays flagged.

Each removed flag is retained in report.json's reviewed_false_positives with a
one-line reviewed_reason. There is no blanket ignore list: mismatched promoted
float/double/DI/TI, argument counts or return types cannot satisfy these rules.
The strict scalar/return guards still audit all calls independently. Remaining
595 flags are actual declaration/definition ABI-class/count disagreements,
not a claim that all 595 are observable gameplay bugs. No unknown callee is
silenced; unresolved targets remain separately reported.

Full strict 3,797-unit audit/manifest check passes: 595 real flags, 81 reviewed
false positives, 60 permutations. Negative/positive ABI fixtures pass, and
configure + ninja + separate check_image print eight-section MATCH. No source
unit or port patch changes in this checker-only commit.


## Round 10: collision hit/sweep adapters and batch records

Fifteen units share the hit query scheduling adapters and spatial_batch.h.
The batch's 0x28-byte SpatialBatchQuery shares the input vector/radius/range,
matrix pointer, flags, hit capacity/buffer and output count. 00214BA0 returns
the total number of hits, fixing eight void caller flags. Existing caller
records occupy all ten words; 00157BE8's 0x180 hit buffer holds eight 0x30 hits.
MIPS discarded-return forms retain schedules, while native calls use the int
API. Canonical matrix-pointer storage replaces the callee's raw x14 integer.

Two shared 00214A18 forms and the batch's 00210CA8 form forward the same EE
bank operands explicitly. All ten related permutations retire; the separately
owned 00214A18.c remains untouched. Full image MATCH, 15/15 native units
compile. Real flags 595 -> 587 (81 reviewed false positives unchanged),
permutations 60 -> 50; no patch change.


## Round 10: particle spawner scheduling APIs

Four caller units move the remaining 0022CDC0/0022CFB8 scale-first declarations
into particle.h. Shared adapters forward scale/effect/direction/matrix operands
in their canonical order; MIPS aliases keep their retail schedules. All ten
remaining spawner permutations retire, with no new prototype guess or patch.
Full image MATCH, 4/4 native units compile. Real flags 587 unchanged, reviewed
false positives 81; permutations 50 -> 40.


## Round 10: ground-distance probe record and API

Nineteen units share ground_probe.h and its full 16-byte quadword/four-float
input/output union. The matching callee now uses that record; native callers
all reach float(pointer, pointer, int flags, float height) through explicit
view/order adapters. Six height-first/between permutations retire. The existing
MIPS declarations/call setup remain under compatibility branches, since the
native order cannot replace those retail schedules. Local normal buffers
inspected in the converted callers have four lanes, not three.

A native fixture exercises every order form repeatedly, verifies flags/height
and all four output lanes, and consumes each float return. Full image MATCH;
19/19 native units compile. Real flags 587, reviewed false positives 81;
permutations 40 -> 34. No patch change; no new live physics causation claimed.


## Round 10: Animation weight API

3 units share animation_weight.h for 00203B68.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 5 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 3/3 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 34 -> 29.


## Round 10: rider proximity query APIs

7 units share rider_queries.h for 0011B268, 0011C0D0, 0011C168.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 6 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 7/7 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 29 -> 23.


## Round 10: animation track search API

4 units share animation_track_search.h for 00163C58.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 3 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 4/4 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 23 -> 20.


## Round 10: spatial primitive query APIs

7 units share spatial_primitives.h for 00211390, 00211BE0, 00213098.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 5 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 7/7 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 20 -> 15.


## Round 10: particle emission control APIs

8 units share particle_emission.h for 00162690, 001B5DE8, 0020C0A0.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 6 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 8/8 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 15 -> 9.


## Round 10: sound fade and pacing APIs

3 units share sound_fades.h for 001CF5C8, 001D3D20.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 2 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 3/3 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 9 -> 7.


## Round 10: motion interpolation APIs

4 units share motion_interpolation.h for 0012D660, 001DD218.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 2 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 4/4 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 7 -> 5.


## Round 10: UI region and color APIs

5 units share ui_region.h for 0019C868, 0020ED30.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 3 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 5/5 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 5 -> 2.


## Round 10: animation time query API

2 units share animation_query.h for 00206E48.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 1 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 2/2 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 2 -> 1.


## Round 10: four-lane vector scale API

2 units share vector_blend.h for 00223578.
Canonical native declarations/implementations retain the reviewed argument and
return classes, with explicit pointer projections into existing field views.
The original declarations/setup remain on MIPS where required for matching;
native adapters spell out the EE-bank order and remove 1 permutations.
Known shared vector/rider/animation/particle identities are reused; opaque
parameter identities do not claim all remaining private field layouts are merged.
The native scalar catalog already fixes every callee's recompiled bridge, so
renaming the native C implementation through its retail asm label does not
introduce a signature-cache guess. No algorithm or patch changes.

Full image MATCH, 2/2 native compiles and scoped float/DI/TI
guards pass. Real flags 587 -> 587; 81 reviewed false positives;
permutations 1 -> 0.


## Round 10: SIF copy completion return

Two units share sif_dma.h's 16-byte request record and 002428E0's int byte-count
return. The eight concatenation/copy calls previously declared void. A shared
MIPS discard alias preserves their schedules; native calls discard a correctly
declared result. The callee and all four request fields match unchanged.
Full image MATCH, 2/2 native compiles. Real flags 587 -> 579; 81 reviewed false
positives; zero permutations and no patch changes.


## Round 10: global load queue processing API

Five units share load_queue.h. 00230F60 invokes the global queued-file processor
00231230 and consumes no context pointer; the latter reads the queued names,
streaming counters and double-buffer state. Shared compatibility forwarding
retains the callers' ignored pointer setup on EE, while native uses void(void).
This is load processing, not a race/physics reset. Full image MATCH, 5/5 native
compiles; eight arity flags retire: real flags 579 -> 571, 81 reviewed false
positives, zero permutations. No patch change.


## Round 10: track projection return, position and angle output

Seven rider/NPC units share track_projection.h. 00119180 returns an int
segment-transition status, correcting eight void declarations. Its fourth
argument is an output address: 002225A8 writes three Euler-angle floats there,
not a fourth-lane direction vector. The retained int-address caller is now an
explicit native bridge. 00157590's +50 prefix grows from char[1] to the proved
12-byte TrackProjectionAngles; 00157498's position prefix grows from twelve
to sixteen bytes. No claim is made that those pointer-only old prefixes were
allocated as undersized standalone objects in the live game.

The native pointer/address fixture writes all three angle lanes and verifies
adjacent canaries. Full image MATCH, 7/7 native compiles; real flags 571 -> 563,
zero permutations. The full audit additionally discovers one exact SI macro
call previously absent from source spans: reviewed false positives now 82
(77 no-prototype SI calls and five packed Color calls).

### Round 10 final counters and validation

143 distinct source units converted. All 60 remaining permutations retire
through shared typed native contracts/adapters, preserving EE integer/FPR bank
order and original MIPS schedules. The largest source commit is one menu API
family (60 units); the rest remain themed families and checker/fixture commits.
Opaque pointer identities retain private field views where full layouts are
not proved; they are not falsely counted as complete class reconstruction.

Flags 696 -> 563: 52 real declaration/count flags fixed, 81 original false
positives moved to reviewed records. Macro expansion locations are now audited
and reveal one additional reviewed false positive. Current report: 563 real
flags and 82 false positives with one-line reasons; all remaining real flags
are 304 return disagreements (244 void->int, 60 int->void) or 259 count/variadic
disagreements. There are no remaining parameter-order/ABI flags. These are
contract disagreements, not 563 proven observable runtime bugs. Unknown
callees remain separately unresolved, never blanket-suppressed.

Full 3,797-unit strict audit/manifest check passes: 19,488 direct calls compared,
zero permutations, zero float return/argument, DI or TI guard errors. The full
normal native build compiles 3,797/3,797 units. The complete runtime suite exits
0, including shared menu/probe/angle fixtures, macro/Color review regressions
and existing rider/NPC/x87/graphics/audio tests. Standalone integer fixtures
also inline animation_query.h before transformation; dependency integration
is committed separately. Configure + ninja and separate check_image print
all-eight-section MATCH before every commit. The full port links 725 recompiled
functions with the existing 23 SDK stubs, zero duplicates and no unresolved
symbols after stubs. No guard or match check weakened.

Zero task-owned patch entries/operations added or removed. Concurrent port
fixes make actual HEAD patches.py 43 entries / 65 text operations; all 65 apply
exactly once in order to committed source. Those fixes/diagnostics are not
staged by this worker. The three protected CRLF-only units remain untouched.

Next: remaining flags by count: 001BDDA0 / 0014E9D0 / 001B4618 (seven each),
then 0018F278 / 0018F210 / 001790E8 / 00187808 (six each). Preserve the zero
permutation baseline and proof-driven false-positive rules. Stop for review.

### Round 11: rider event submission

001BDDA0: void caller declarations -> int result/index; the callee returns -1 on rejection and the selected queue index otherwise. Shared RiderEventState defines all five words, including the submission clock at +0x10. Matching callers retain a shared void scheduling alias; native callers use the int contract. The actor remains an opaque pointer because its private virtual layouts have not been unified. Seven flagged calls in four rider units repaired.

Real flags 563 -> 556; reviewed false positives 82 -> 82; gameplay callee-range flags 296 -> 289. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: global entry predicate

0014E9D0: int(pointer) -> int(void). It compares the globally selected index with the last entry index; the context pointer is never read. Seven calls in five units now share the no-argument native predicate, with a shared EE legacy context alias.

Real flags 556 -> 549; reviewed false positives 82 -> 82; gameplay callee-range flags 289 -> 282. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: render synchronization

001B4618: void(pointer) -> void(void). The implementation waits on the three global pending flags and calls func_002433A8(0,0); it never reads a context. Seven flagged calls now share the canonical no-argument interface and an explicit EE legacy context alias. Includes the labelled call inside 001EB1D8.

Real flags 549 -> 542; reviewed false positives 82 -> 82; gameplay callee-range flags 282 -> 275. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: track bin search outputs

00117A78: int -> void; its forwarded callee 00117A98 is also void, writing short indices and optional float distance. Shared PathBins and TrackBinNode replace the private record definitions. Five caller returns now agree with the true void wrapper, and the wrapper no longer declares its void callee int. Pointer-address arguments and signed short outputs are explicit; no new live overflow or physics causation is claimed.

Real flags 542 -> 536; reviewed false positives 82 -> 82; gameplay callee-range flags 275 -> 269. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: motion key sampling

00167A00: five void callers -> int selected-key result. All seven units share the six-argument contract, including the last float time and float output pointers. Matching discard callers use a shared EE alias; native callers preserve the int return contract. This touches rider/physics trajectory sampling (0011C918, 00128DD8, 0016B400, 0016C720) and rider sound-value sampling (00195F90); no new delta-time interpretation is asserted.

Real flags 536 -> 531; reviewed false positives 82 -> 82; gameplay callee-range flags 269 -> 264. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: derived menu construction

0018F210: native C++ void constructor -> explicit pointer-return native construction contract, matching retail this in v0; six original flagged calls are repaired. SharedDerivedMenu is a complete 0x50C native view with the base Menu at +0 and embedded member at +0x504. Native construction explicitly calls the shared base initializer, then performs the same writes; retail keeps its original C++ constructor for MATCH. All direct C callers use the shared native constructor. The executable fixture verifies base-call chaining, member/link offsets, vtable pointers and surrounding canaries. The private derived C++ layouts of 0019B740 are not unified here.

Real flags 531 -> 525; reviewed false positives 82 -> 82; gameplay callee-range flags 264 -> 258. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: global menu refresh

00187808: unprototyped or context-taking declarations -> void(void). The body reads and restores global menu selection fields, without reading a passed object. Six context-bearing calls now share the explicit no-argument native routine and legacy EE context view; the no-argument call also uses the prototype.

Real flags 525 -> 519; reviewed false positives 82 -> 82; gameplay callee-range flags 258 -> 252. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: empty menu finalizer

0018F278: void(pointer) -> void(void), including both asm-labelled aliases in 0018B3A8. Retail body is empty and never reads the object. Six flagged calls now use the canonical no-argument native routine via the shared matching alias.

Real flags 519 -> 513; reviewed false positives 82 -> 82; gameplay callee-range flags 252 -> 246. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: player slot event state

001790E8: seven arguments -> six (object plus five integers). Shared PlayerSlotEvent records the touched offsets through +0x12C0; the trailing seventh integer is never read. Six calls in five units now forward only the consumed native operands, retaining a shared EE seven-argument alias. Padding is an opaque prefix, not a reconstructed full player object.

Real flags 513 -> 507; reviewed false positives 82 -> 82; gameplay callee-range flags 246 -> 240. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: rider AI update

00124B40: void(object,targetVector) -> void(object). The target parameter is not part of the definition and is never read. Three callers in 00123350, 00125178 and 001264D8 now share the single-object native interface; EE calls retain the second operand through a shared matching alias. The AI/rider object stays opaque; this does not reconstruct its skeleton or assert a runtime matrix fix.

Real flags 507 -> 504; reviewed false positives 82 -> 82; gameplay callee-range flags 240 -> 237. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: RTC output forwarding

00193B00: void(void) dropping the live output pointer -> int(RtcClock *), forwarding that pointer and the SDK status. 00248058 is the SDK clock reader: retail saves a0 in s2, copies exactly eight bytes through s2, and returns integer RPC status. Five original flags repaired; all seven native units use rtc.h. Recompiler catalogues explicitly pin both SI signatures even with an old cache; native-contract verification guards the wrapper. The executable fixture exercises 96 direct/legacy forwards, nonzero status, all date fields and output canaries. 00222E10 uses the shared eight-byte clock record when deriving its seed. This is RTC/date/seed data, not race elapsed time.

Real flags 504 -> 499; reviewed false positives 82 -> 82; gameplay callee-range flags 237 -> 232. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

RTC identification also uses the primary [ps2sdk clock record](https://github.com/ps2dev/ps2sdk/blob/master/common/include/libcdvd-common.h) and [read-clock RPC implementation](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/cdvd/src/scmd.c): eight BCD bytes (status/second/minute/hour/padding/day/month/year), RPC command 15, eight-byte copy, integer status. Only declarations/field facts are used; no SDK implementation code is imported.

Port worker additions for round 11: 001BDDA0 rider event result/state and submission clock; 00117A78/00117A98 track-bin indices/output contract; 00167A00 rider trajectory/key sampling result; 00124B40 rider AI single-object contract. RTC forwarding fixes date output/seed data, without asserting a link to the race-reset symptom. 0022B028 remains a velocity multiplier; no new caller evidence changes that interpretation.

### Round 11: load transition initialization

001B4A60: four void callers -> returned state pointer. LoadTransitionState shares the complete constructor extent (0x64) and the touched fields; private subrecord padding stays opaque. Four callers in load/menu transitions use the shared discard view while native calls preserve the pointer result. Existing 0x70 caller buffers remain large enough; no new overflow is claimed.

Real flags 499 -> 495; reviewed false positives 82 -> 82; gameplay callee-range flags 232 -> 228. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: derived constructor retail return

Retail 0018F210 +0x38 explicitly copies this (s0) to v0. The complete native initializer now returns SharedDerivedMenu * instead of reproducing the i386 C++ void constructor ABI. Original pointer-return callers were correct about the EE result. The earlier six checker flags came from the native constructor definition; final native calls/results now preserve the retail pointer. The fixture asserts the returned pointer, and the reviewed SI bridge pins one pointer argument and pointer result.

Real flags 495 -> 495; reviewed false positives 82 -> 82; gameplay callee-range flags 228 -> 228. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11: reviewed SI bridge cache contracts

The recompiler cache still held the old one-pointer render wait and integer-only constructor guess. Eleven additional reviewed SI contracts now override a current cache unconditionally, including no-argument APIs, track-bin outputs, rider AI and player-slot argument count. Existing native-definition verification rejects stale metadata. A synthetic current cache with eight arguments, variadic signatures and wrong returns must be replaced by all eleven canonical shapes. Flags/classification stay unchanged; this closes asm/native cache disagreements outside the float/DI/TI classes.

Real flags 495 -> 495; reviewed false positives 82 -> 82; gameplay callee-range flags 228 -> 228. Zero permutations; zero owned patch changes. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 11 final review handoff

65 distinct owned source units, 12 shared API families. Real flags 563 -> 495
(68 repaired); reviewed false positives 82 -> 82 (none newly suppressed).
Gameplay callee-range flags 296 -> 228, counting each flagged call whose callee
address is in [0x110000, 0x1C0000). The remaining 495 real flags are 276 return
and 219 count/variadic disagreements; zero other parameter ABI flags.
The checker now writes these classified counts and prints the gameplay subset;
boundary/non-address/static-symbol tests protect the range calculation.
These are contract disagreements, not 495 proven observable runtime bugs.
Permutations remain 0 -> 0. Float returns/arguments, DI and TI guards all remain
zero; no ABI guard or matching check was weakened.

The significant extra fix is 00193B00: restore the eight-byte RTC output pointer
and integer SDK status at the native/recompiled boundary. Retail 0018F210
returns this in v0; its explicit native initializer now returns the same shared
menu pointer, instead of the native C++ void constructor result. Eleven new SI
bridge shapes plus the RTC wrapper/SDK reader override stale recompiler caches.
The fixture covers stale cache replacement, RTC writes/status/canaries and
constructor return/base chaining/extent. Standalone integer fixtures also inline
motion_sample.h before transformation and symbol prefixing.

Final configure + ninja and separate check_image: all eight sections MATCH.
Every touched unit compiles natively; final full normal build 3,797/3,797, zero
errors. Full strict ABI/manifest audit passes (19,489 calls, zero permutations),
and the complete runtime suite exits 0. Full port link exits 0: 4,545 objects,
847 C++ symbols renamed, 725 asm functions recompiled, with the existing 23 SDK
stubs, no duplicates and no unresolved symbols after stubs.
Zero task-owned patch entries or text operations added/removed; all 43 committed
patch entries / 65 text operations still apply exactly once, in order, to HEAD
source. Renderer/audio/proof diagnostics and protected CRLF-only units are not
staged. Validation logs and owned commit list: build-try/shared-round11/.

Next: 001852A8 (four flags), then gameplay 00111150, 0014D948/0014D9C0,
0017EDE8 and 00184D20 (three each), prioritizing rider/physics/input meanings.
Then the five-call families 002318C0, 00223A98, 002013F8 and remaining real
flags by count. Keep the explicit native/recompiled SI signatures in sync,
review actual retail returns (including constructors), and retain classified
counts. Round 11 complete; stop for review.

### Round 12: rider and VIF matrix helpers

002239B0: two pointer-return calls -> true void composition into a 64-byte matrix. 00223A38/00223A68: integer declarations -> void matrix copy/identity; address-taking EE views are explicit aliases and native forwards use Matrix4. All seven units share the contracts, including the rider-space 00130010 and VIF packet 00215B68. The formerly unused result operand in 00130010 gets the actual destination in the native compatibility view; it remains the original scheduling view on EE. Its two local matrices are explicitly 16-byte aligned, still MATCH. Executable copy/overlap, identity, composition, aliasing and 64-byte output canaries pass at O1. Native-definition/recompiler SI contracts are pinned. One owned body-patch entry removed (00223A38): its native memmove lives beside the unchanged EE assembly and is committed in the same hunk boundary. No live shard cause is established.

Real flags 495 -> 489; reviewed false positives 82 -> 82; gameplay callee-range flags 228 -> 228. Particle-range flags 26 -> 20; zero permutations. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 12: particle descriptor and spawn result

0022D1B0: void actor call -> int allocated-particle index/-1. All four units now share the four-pointer native contract, including traversal, rider actor and VIF packet calls. ParticleEmitterDescription unifies the complete 0xB0 descriptor with its 64-byte matrix at +0x60, vector at +0xA0 and explicitly DI-width state/active flags at +0x58; previously the spawner used a 16-byte matrix prefix plus padding while the packet caller used the full matrix. Flag bits 32/36 and descriptor layout/canaries are exercised in a native forwarding fixture. Retail body keeps its scheduling pointer views; native uses the shared prototype. No patch changes in this family and no live shard causation asserted.

Real flags 489 -> 488; reviewed false positives 82 -> 82; gameplay callee-range flags 228 -> 228. Particle-range flags 20 -> 19; zero permutations. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.

### Round 12: three-section particle spray emitters

0023F650/0023FAE8: three caller arguments -> two (emitter and position); retail overwrites a2 with the position quad before reading the third input. Both flags in particle updater 00228BE8 are repaired. The complete 0x2860 ParticleSprayEmitter view and shared vector union own the vertex/velocity/short-lifetime/color buffer types, used by the actual emitter definitions. The updater retains its partial field view with typed forwards. Actual bodies execute in a fixture for both variants, two ring slots, three sections, finite vertices, expected colors and short lifetimes, and all output canaries. The ignored third integer has no influence. Native SI contracts override stale three-argument cache guesses. No patch changes; this is not proof that the live shard symptom is fixed.

Real flags 488 -> 486; reviewed false positives 82 -> 82; gameplay callee-range flags 228 -> 228. Particle-range flags 19 -> 19; zero permutations. Gameplay subset counts calls whose callee address is 0x110000 <= address < 0x1C0000; each flagged call counts once. Source/retail disassembly are the evidence. Full configure + ninja + separate check_image: MATCH; each owned unit compiles natively and scoped scalar guards pass.
