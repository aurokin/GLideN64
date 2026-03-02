# N64 Runtime Validation Checklist

## Objective

Define a Vulkan validation loop that treats N64 docs and hardware behavior as ground truth, not legacy GL plugin output.

## Canonical references

- Local bundle: `docs/references/n64/README.md`
- Primary docs:
  - `docs/references/n64/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf`
  - `docs/references/n64/SGI_Nintendo_64_RSP_Programmers_Guide.pdf`
  - `docs/references/n64/SGI_RDP_Command_Summary.pdf`
  - `docs/references/n64/allman51eng.zip`
  - `docs/references/n64/n64brew_Reality_Display_Processor_Commands.html`
  - `docs/references/n64/n64brew_Reality_Display_Processor_Pipeline.html`
  - `docs/references/n64/n64brew_Reality_Signal_Processor.html`
  - `docs/references/n64/n64brew_Video_Interface.html`

## Hardware-contract checklist

Mark each item for every bugfix or parity patch:

1. RDP command semantics
- Verify command decode/parameter interpretation against RDP command docs.
- Confirm texrect/triangle command paths handle coordinate precision and clamping as documented.

2. TMEM and tile behavior
- Validate address wrap/mask/shift behavior, including TMEM overflow wrap cases.
- Validate palette/format/size decoding and tile descriptor edge cases.

3. Framebuffer and copy semantics
- Validate color/depth image writes and copy paths against RDP/VI behavior.
- Verify auxiliary buffer interactions (pause overlays, transitions, RDRAM reads/writes).

4. Depth, blend, coverage, dither
- Validate coverage/depth compare rules and update order.
- Validate blend path assumptions against documented constraints.
- Validate dither/noise behavior only where game-visible and spec-backed.

5. VI post-processing and output
- Validate VI scaling/filter path assumptions separately from RDP correctness.
- Confirm interlace and low-resolution output behavior for affected titles.

6. Host integration contract
- Validate non-default/default framebuffer handling across host adapters.
- Verify lifecycle: startup, state load, pause/step/capture, shutdown.

## Runtime validation flow (Linux)

1. Build and gate

```bash
REALITYVK_GRAPHICS_BACKEND=Vulkan ./scripts/local_gate.sh
```

2. Run focused backend parity matrix (Paper Mario checkpoints)

```bash
REALITYVK_SMOKE_MANIFEST=tests/smoke/scenarios_paper_mario_runtime.tsv \
REALITYVK_SMOKE_BACKENDS='Reference Candidate' \
REALITYVK_SMOKE_REFERENCE_BACKEND=Reference \
REALITYVK_SMOKE_STRICT_BASELINE=0 \
REALITYVK_SMOKE_REPEAT_COUNT=2 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=1 \
REALITYVK_SMOKE_PLUGIN_REFERENCE=build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
REALITYVK_SMOKE_PLUGIN_CANDIDATE=build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
./scripts/local_smoke.sh
```

Majority mode (less strict, still deterministic):

```bash
REALITYVK_SMOKE_REPEAT_COUNT=3 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=0 \
REALITYVK_SMOKE_REQUIRE_MAJORITY=1 \
./scripts/local_smoke.sh
```

3. Classify any divergence
- Spec mismatch candidate
- Known legacy workaround mismatch
- Host/runtime issue
- Flaky/non-deterministic capture

4. Log and track
- Add scenario notes directly to the active status tracker (`docs/vulkan-core-status.md`).
- If reproducible, keep it in smoke manifest or create a new targeted manifest.

## Current focus checkpoints

- `boot_title_003`: offscreen path health and framebuffer correctness.
- `prologue_castle_stairs_party_002_fresh`: currently unstable/flaky capture checkpoint.
- `prologue_castle_upperhall_right_003_fresh`: live progression sanity check (also intermittently unstable).
