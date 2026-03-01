# Runtime Validation Report (2026-03-01)

## Scope

- Manifest: `tests/smoke/scenarios_paper_mario_runtime.tsv`
- Backends: `Reference`, `Candidate` (both executed on Vulkan runtime path)
- Plugin: `build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so`
- Runtime root: `/home/auro/code/mupen`

## Commands executed

1. Backend parity without baseline lock-in:

```bash
REALITYVK_SMOKE_MANIFEST=tests/smoke/scenarios_paper_mario_runtime.tsv \
REALITYVK_SMOKE_BACKENDS='Reference Candidate' \
REALITYVK_SMOKE_REFERENCE_BACKEND=Reference \
REALITYVK_SMOKE_BASELINES=/tmp/empty-smoke-baselines \
REALITYVK_SMOKE_STRICT_BASELINE=0 \
REALITYVK_SMOKE_OUTPUT=build/smoke-runtime-matrix-f1-nobase \
REALITYVK_SMOKE_PLUGIN_REFERENCE=build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
REALITYVK_SMOKE_PLUGIN_CANDIDATE=build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
./scripts/local_smoke.sh
```

2. Stability enforcement run:

```bash
REALITYVK_SMOKE_REPEAT_COUNT=2 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=1 \
... \
./scripts/local_smoke.sh
```

3. Exploratory repeat run allowing instability:

```bash
REALITYVK_SMOKE_REPEAT_COUNT=2 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=0 \
... \
./scripts/local_smoke.sh
```

## Outcomes

1. Parity signal
- The focused parity run can pass with exact reference/candidate checksum parity on all 7 scenarios.

2. Determinism signal
- Repeated capture with stability enforcement surfaced nondeterminism in live checkpoints.
- Example failure:
  - `pm_live_stairs_party_002_fresh` (Vulkan) alternated between:
    - `472183a648ff259d3b5e65a980068aad318d305433f4badb57f76b41588ff06b`
    - `0539e7cc6590ceef1ef6e7fe882378d19029d73db2b77107673a8e0d76741e31`

3. Cross-backend instability coupling
- In some runs, reference and candidate converged to the same hash cluster.
- In other runs, they landed on different valid clusters for `pm_live_stairs_party_002_fresh` and `pm_live_upperhall_right_003_fresh`.

## Current interpretation

- Remaining blocker in this matrix is now primarily capture determinism, not a single consistently reproducible Vulkan-only mismatch.
- `REALITYVK_SMOKE_REPEAT_COUNT` and `REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE` are now required controls for strict local gating.

## Next validation step

- Capture/step policy hardening for live checkpoints:
  - either increase frame settle policy per scenario, or
  - add multi-sample consensus logic at scenario level before backend compare.
