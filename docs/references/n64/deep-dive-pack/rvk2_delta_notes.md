# RVK2 Delta Notes From Deep-Dive Pack

Date: 2026-03-02

Scope reviewed:
- `deep-dive-pack/report.md`
- `deep-dive-pack/docs/links/curated_links.md`
- `deep-dive-pack/docs/extracted/cheatsheets/*`
- `deep-dive-pack/docs/extracted/libdragon_src_display.c`
- `deep-dive-pack/docs/extracted/repeater64_Readme.md`
- `deep-dive-pack/docs/extracted/parallel-rdp_README.md`

## What This Pack Adds (High Value)

1. Hidden RDRAM bit is treated as a first-class validation target.
   - Reinforces that coverage correctness is not just final RGBA; hidden/coverage planes matter.
2. VI behavior has practical, testable edge semantics.
   - Mid-frame VI register effects are explicitly highlighted (`repeater64` pre-line effects).
   - Field/interlace behavior is concretely exercised in `libdragon` display flow (`VI_V_CURRENT` field bit + serrate).
3. Better external conformance/test strategy references.
   - `rdp-conformance`/`vi-conformance` and replay workflows from parallel-rdp notes.
   - Stress ROM targets are clear (`repeater64`, `n64-systemtest`).

## Where Our Prior Mental Model Was Incomplete

1. We underweighted VI mid-frame register mutation as a blocker for "recognizable motion".
2. We treated some paraLLEl implementation shortcuts as if they were normative hardware behavior.
   - Pack explicitly labels several as intentional deterministic deviations for practicality.
3. We focused on RDP color path before hidden-bit parity strategy was formalized.
   - Pack pushes hidden-bit + coverage as a co-equal validation surface.

## New/Previously Underused Leads

1. Repeater64 hard cases:
   - RDP fill-mode triangles
   - fill-mode sync abuse
   - test-mode span-buffer read/write behavior
   - VI pre-line effects
2. libdragon VI configuration caveats worth explicit reproduction checks:
   - invalid filter combinations handling
   - 16bpp/AA mode caveat at low width in NTSC contexts
   - field-sensitive behavior in interlaced output logic

## How To Apply In RVK2 Next

1. Add explicit "hidden coverage plane parity" checks in debug/conformance outputs.
2. Add a VI mid-frame update micro-suite (line-shift/scale deltas) to smoke/conformance.
3. Gate cycle2/coverage fixes against targeted repeater64 scenarios before broad parity sweeps.
4. Treat parallel-rdp README "intentional differences" as non-authoritative when resolving N64 semantics.
