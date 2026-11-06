# 170: PowerVR GE8320 phones close at start

Status: in progress

Branch `fix/powervr-crash` from `main` fce8a1f (0.7.12), started 2026-11-06. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issues #56 (OPPO CPH2269, Android 11) and #60 (Infinix Hot 11 Play): phones with a PowerVR Rogue
GE8320 GPU (Helio G35/P35-class chips; driver "PowerVR Rogue Vulkan Driver 1.13@5776728") close
right after the renderer starts. The shared log (Share log, finding 152; kept by the lead) shows
`crash: SIGSEGV at 0000000000000008 in thread game` with the frames inside the PowerVR driver's
`libufwriter.so` (its shader compiler / USC code writer), right after
`gs: renderer dhd_render 1.0 wgpu 29.0.3 ... on PowerVR Rogue GE8320`. Finding 153 fixed a similar
start crash in Mali's r54 compiler (no texture passed into a shader function).

Goal: the game starts and plays on these phones with the native renderer, one code path for every
GPU (a narrowly scoped driver workaround only if no common form works), pictures unchanged on other
GPUs (`test_gs_wgpu`, the native pictures byte-identical where the shaders change).

There is no PowerVR phone here. Find the cause from evidence: which pipeline is built when it
crashes (the log has no pipeline trace; finding 153's test patch logged each pipeline before
vkCreate*Pipelines), what PowerVR's compiler is known to choke on (wgpu/naga issues, other
emulators' PowerVR workarounds: PPSSPP, Dolphin, DuckStation, Skyline/yuzu-android, Vita3K, PCSX2
Android forks; Imagination's offline compiler PVRShaderEditor/`glslangValidator`-based tools if
they can compile SPIR-V for Rogue), and our SPIR-V (`port/render/tests/native_spirv.rs` writes every
stage as wgpu-hal makes it). Rank the candidates, implement the most likely common fix, and prepare
a test APK for players: release-signed (signing variables from the lead's notes, never print the
passwords), with the pipeline trace so a tester's Share log names the crashing pipeline if it still
crashes.

Acceptance: the cause or the ranked candidates with evidence; the fix; `check.py port` passing;
the shaders' SPIR-V checks (`native_spirv`); a test APK path for the lead (a release candidate
build plus the trace patch on a local test branch); on the Mi 11i (Adreno) the test APK starts and
draws the title (the lead or the lane, phone rules apply).

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
