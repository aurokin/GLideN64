# Local Smoke Gate

This project now has a deterministic smoke gate script:

```bash
./scripts/local_smoke.sh
```

Paper Mario parity wrapper (reference vs candidate plugin):

```bash
./scripts/paper_mario_parity.sh
```

One-command compare + viewer refresh (captures, composes, closes previous image, opens latest):

```bash
./scripts/paper_mario_compare_view.sh
```

Useful compare-view env vars:

- `REALITYVK_PM_COMPARE_VIEW_MODE`:
  - `triptych` (default): reference + candidate + diff.
- `REALITYVK_PM_COMPARE_VISUAL_GATE`:
  - `0` (default): always produce compare image, even when metrics exceed gate limits.
  - `1`: enforce parity gate during compare-view run.
- `REALITYVK_PM_COMPARE_VIEWER`:
  - Viewer command (`eog` default; falls back to `display`/`xdg-open` when unavailable).

It is designed to run with the local Paper Mario LLM runtime in:

- `/home/auro/code/mupen`

Default runner:

- `./scripts/paper_mario_smoke_runner.sh`

## What it checks

For each configured backend/scenario:

1. Run emulator through the agent-capable runtime.
2. Capture a deterministic framebuffer preset.
3. Hash capture output with SHA-256.
4. Compare results either:
- against committed baseline checksums, or
- against a reference backend from the same run.

## Scenario manifest

Default file: `tests/smoke/scenarios.tsv`

Columns (tab-separated):

1. `scenario_id`
2. `rom_path`
3. `frames`
4. `scenario_args` (optional runner args)

Example row:

```tsv
paper_mario_intro	/home/auro/code/paper_mario/baseline/papermario-built.z64	120	--preset full --scale-div 2
```

## Baselines

Baselines live in:

- `tests/smoke/baselines/<Backend>.checksums.tsv`

Update baselines:

```bash
REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh
```

## Common env vars

- `REALITYVK_SMOKE_BACKENDS`:
  - Example: `Vulkan`
- `REALITYVK_SMOKE_REFERENCE_BACKEND`:
  - Default: `Vulkan`
  - Non-reference backends are diffed against this backend's current run output.
- `REALITYVK_SMOKE_MANIFEST`
- `REALITYVK_SMOKE_OUTPUT`
- `REALITYVK_SMOKE_TIMEOUT_SEC`
- `REALITYVK_SMOKE_PLUGIN`:
  - Default plugin binary used by runner.
- `REALITYVK_SMOKE_PLUGIN_VULKAN`
- `REALITYVK_SMOKE_AGENTCTL_TIMEOUT_SEC`:
  - Per-agent command timeout in seconds inside the runner.
  - Default: `30`
- `REALITYVK_SMOKE_STEP_CHUNK`:
  - Number of frames per `agentctl step` call inside runner.
  - Default: `120`
  - Increase/decrease for stability when scenarios need large frame advances.
- `REALITYVK_SMOKE_SETTLE_FRAMES_AFTER_LOAD`:
  - Extra frames to step immediately after loading a savestate, before scenario stepping/capture.
  - Default: `1`
  - Helps stabilize state-based checkpoint captures by enforcing a consistent post-load settle window.
- `REALITYVK_SMOKE_REQUIRE_BACKEND_PLUGIN=1`:
  - Require `REALITYVK_SMOKE_PLUGIN_<BACKEND>` to be set for the selected backend.
  - Useful to avoid accidental backend/plugin mismatches during parity work.
  - When more than one backend is selected, this is enabled automatically.
- `REALITYVK_SMOKE_REQUIRE_READBACK_MARKER`:
  - `0` (default): no marker enforcement.
  - `1`: require Vulkan readback marker logs from plugin runtime (`VK readback debug: ...`), failing smoke if absent.
  - Intended as regression protection for `ColorBufferReader`/`PixelReadBuffer` Vulkan paths.
- `REALITYVK_SMOKE_READBACK_MARKER_REGEX`:
  - Regex used when marker enforcement is enabled.
  - Default: `VK readback debug:`
- `REALITYVK_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL`:
  - `0` (default): do not enforce depth-blit failure markers.
  - `1`: fail smoke if launch log contains depth blit failure markers (`op=blit_depth_fail`).
- `REALITYVK_SMOKE_REQUIRE_DEPTH_BLIT_STATS`:
  - `0` (default): depth stats marker optional.
  - `1`: require at least one `depthStats=[attempts=... success=... fail=...]` marker in launch log.
- `REALITYVK_SMOKE_DEPTH_BLIT_FAIL_REGEX`:
  - Regex used to detect depth blit failures.
  - Default: `op=blit_depth_fail`
- `REALITYVK_SMOKE_DEPTH_BLIT_SUMMARY_OUT`:
  - Optional JSON output path for per-run depth blit summary (fail counts, reasons, and last stats marker).
- `REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE`:
  - `0` (default): accept capture file as long as it exists.
  - `1`: reject captures that are effectively black and retry stepping/capture before failing.
- `REALITYVK_SMOKE_CAPTURE_RETRY_COUNT`:
  - Number of additional capture retries when non-black enforcement is enabled.
  - Default: `6`
- `REALITYVK_SMOKE_CAPTURE_RETRY_STEP_FRAMES`:
  - Extra frames to step between retries when capture appears black.
  - Default: `20`
- `REALITYVK_SMOKE_CAPTURE_RETRY_RESUME_MS`:
  - Real-time resume duration (milliseconds) before each retry capture when black-frame validation fails.
  - Helps backends that paint only while running (not paused-step only).
  - Default: `250`
- `REALITYVK_SMOKE_CAPTURE_MIN_NONBLACK_RATIO`:
  - Minimum fraction of non-black pixels required for a valid capture.
  - Default: `0.001`
- `REALITYVK_SMOKE_CAPTURE_MIN_MEAN_LUMA`:
  - Minimum normalized mean luma (`0..1`) required for a valid capture.
  - Default: `0.002`
- `REALITYVK_SMOKE_CAPTURE_DEBUG=1`:
  - Logs capture-content metrics (non-black ratio, mean luma, thresholds) during validation.
- `REALITYVK_SMOKE_SCREENSHOT_FALLBACK`:
  - `1` (default): when framebuffer dump stays black under non-black enforcement, attempt `agentctl screenshot` fallback.
  - `0`: disable screenshot fallback.
- `REALITYVK_SMOKE_SCREENSHOT_DIR`:
  - Directory scanned for latest screenshot artifact during fallback.
  - Default: `~/.local/share/mupen64plus/screenshot`
- `REALITYVK_SMOKE_FORCE_SCREENSHOT_CAPTURE`:
  - `0` (default): capture via `dumpfb-preset`, with screenshot fallback only when needed.
  - `1`: force capture via core screenshot path for every run.
  - Useful when reference and candidate must use the exact same capture method.
- `REALITYVK_SMOKE_DUMPFB_FLIP_Y`:
  - `0` (default): do not request color capture flip in `dumpfb-preset`.
  - `1`: pass explicit `flip_y=1` request to agent color framebuffer dump commands.
  - No automatic image normalization is performed by smoke scripts.
- `REALITYVK_SMOKE_LAUNCH_WITH_PTY`:
  - `0` (default): launch emulator normally.
  - `1`: launch emulator under a pseudo-terminal (`script`) for plugins/environments that require TTY-like startup behavior.
- `M64_CORELIB`:
  - Optional override for the core library path used by `launch.sh`.
  - Useful when pinning an alternate core build for deterministic parity/smoke runs.
- `REALITYVK_VK_DEBUG_READBACK=1`:
  - Enables readback marker logs inside RealityVK.
  - Usually paired with `REALITYVK_SMOKE_REQUIRE_READBACK_MARKER=1`.
- `REALITYVK_VK_EXPERIMENTAL_FETCH_BLEND=1`:
  - Opt-in switch enabling all experimental fetch/blend feature flags in Vulkan.
  - Equivalent to enabling:
    - `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_DEPTH=1`
    - `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1`
    - `REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1`
  - Default is `0`/unset to keep the stable candidate path.
- `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_DEPTH=1`:
  - Opt-in switch for `FramebufferFetchDepth` capability reporting in Vulkan.
- `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1`:
  - Opt-in switch for `FramebufferFetchColor` capability reporting in Vulkan.
- `REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1`:
  - Opt-in switch for `DualSourceBlending` capability reporting in Vulkan.
  - Requires Vulkan device dual-source blend feature support; otherwise capability remains disabled.
- `REALITYVK_VK_STRICT_FB_FETCH_COLOR=1`:
  - Uses strict framebuffer-fetch-color blend behavior when `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1` is enabled.
  - Default (unset): compatibility fallback keeps ordinary blending.
- `REALITYVK_VK_STRICT_DUAL_SOURCE_BLEND=1`:
  - Uses strict dual-source blend behavior when `REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1` is enabled.
  - Default (unset): compatibility fallback keeps ordinary blending.
- `REALITYVK_VK_DEBUG_STRICT_BLEND_STATE=1`:
  - Logs dominant strict blend state tuples (packed blend mux/params + resolved Vulkan blend factors) at power-of-two counts.
- `REALITYVK_VK_DEBUG_STRICT_BLEND_STATE_LIMIT`:
  - Caps strict blend state log lines (default: `96`).
- `REALITYVK_VK_STRICT_ONLY_PARAMS=<int|hex>`:
  - Applies strict blend mux only to packets with matching packed blend params (debug isolation helper).
- `REALITYVK_VK_STRICT_OFFSCREEN_DISABLE=1`:
  - Disables strict blend mux in offscreen packet execution only (debug isolation helper).
- `REALITYVK_VK_FORCE_OFFSCREEN_RASTER_RECT_TRANSFORM=1`:
  - Restores legacy forced offscreen rect transform behavior for diagnostics.
  - Default (unset): use corrected offscreen normalization path.
- `REALITYVK_VK_DISABLE_RT_TEXRECT_Y_FLIP=1`:
  - Disables the default Vulkan present-path Y-flip correction for texrect packets sampling render-target textures.
  - Keep unset for the current improved baseline path.
- `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_HANDLES`:
  - Comma-separated texture handles to trace in present compositing (for example `13`).
  - Emits focused `VK composite trace(final): ...` logs only for packets that sample one of these handles.
- `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_LIMIT`:
  - Max number of focused final-layer trace lines (default: `96`).
- `REALITYVK_VK_DISABLE_COMBINER_MODULATE_OVERRIDE=1`:
  - Disables the current default combiner constant-modulate override heuristic.
  - Leave unset for the current best Paper Mario intro signal.
- `REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_POS=1`:
  - Disables canonical fullscreen quad positions for final present-path texrect packets that sample RT-backed textures.
  - Leave unset for the current improved baseline path.
- `REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_UV=1`:
  - Disables canonical fullscreen UV assignment for final present-path texrect packets that sample RT-backed textures.
  - Leave unset for the current improved baseline path.
- `REALITYVK_VK_DISABLE_SPECIAL_GAMMA=1`:
  - Debug-only toggle to disable Vulkan gamma special shader flag emission.
- `REALITYVK_VK_DISABLE_SPECIAL_FXAA=1`:
  - Debug-only toggle to disable Vulkan FXAA special shader flag emission.
- `REALITYVK_SMOKE_REPEAT_COUNT`:
  - Number of capture runs per backend/scenario.
  - Default: `1`
- `REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE`:
  - `1` (default): fail if repeated runs for a backend/scenario produce different hashes.
  - `0`: allow instability and choose the modal hash for that scenario.
- `REALITYVK_SMOKE_REQUIRE_MAJORITY`:
  - `1` (default): when `REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=0`, still require a strict majority hash (`> N/2`) across repeats.
  - `0`: allow non-majority modal selection (exploratory use only).

## Backend parity safety

When `REALITYVK_SMOKE_BACKENDS` contains multiple backends, `local_smoke.sh` requires
explicit backend plugin mapping via:

- `REALITYVK_SMOKE_PLUGIN_<BACKEND>`

This prevents false parity passes caused by reusing the same plugin binary for every backend.

## Paper Mario depth summaries

`paper_mario_parity.sh` now supports depth summary artifacts:

- `REALITYVK_PM_CAPTURE_DEPTH_SUMMARY=1` (default): write per-capture summary JSON files under run root:
  - `<scenario>.reference.depth-blit-summary.json`
  - `<scenario>.candidate.depth-blit-summary.json`
- `REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL=1`: enforce strict no-depth-failure gate for candidate capture.
- `REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS=1`: require depth stats marker for candidate capture.
- `REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE=1` (default):
  - Enforce non-black capture validation in Paper Mario parity runs.
- `REALITYVK_PM_CAPTURE_RETRY_COUNT` / `REALITYVK_PM_CAPTURE_RETRY_STEP_FRAMES`:
  - Control retry behavior when a capture is detected as mostly black.
- `REALITYVK_PM_CAPTURE_RETRY_RESUME_MS`:
  - Real-time resume warmup per retry for Paper Mario parity capture.
- `REALITYVK_PM_SCREENSHOT_FALLBACK` / `REALITYVK_PM_SCREENSHOT_DIR`:
  - Control screenshot fallback behavior for Paper Mario parity runs.
- `REALITYVK_PM_FORCE_SCREENSHOT_CAPTURE`:
  - Pass-through to `REALITYVK_SMOKE_FORCE_SCREENSHOT_CAPTURE`.
- `REALITYVK_PM_REFERENCE_FORCE_SCREENSHOT_CAPTURE`:
  - Force screenshot capture for reference only.
  - Default: `1` (avoids upstream-GL dumpfb black-frame path and keeps reference cache method explicit).
- `REALITYVK_PM_DUMPFB_FLIP_Y`:
  - Pass-through to `REALITYVK_SMOKE_DUMPFB_FLIP_Y`.
  - Default for `paper_mario_parity.sh` is `1` (explicit agent flip request) to match live-window orientation.
- `REALITYVK_PM_LAUNCH_WITH_PTY`:
  - Pass-through to `REALITYVK_SMOKE_LAUNCH_WITH_PTY`.
  - Default for `paper_mario_parity.sh` is `1`.
- `REALITYVK_PM_REFERENCE_CORELIB`:
  - Core library used for reference capture.
  - Default: same as `REALITYVK_PM_CANDIDATE_CORELIB`.
- `REALITYVK_PM_CANDIDATE_CORELIB`:
  - Core library used for candidate capture.
  - Default: `/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2`

Per-run Paper Mario parity artifacts now include:

- `<scenario>.metrics.json`
- `<scenario>.diff.png`
- `<scenario>.reference.png`
- `<scenario>.candidate.png`
- `<scenario>.capture-context.json`:
  - Records exact reference/candidate plugin, core library, capture method, and capture file path used for that run.

## Flake hardening

Use repeated capture to detect nondeterministic scenarios:

```bash
REALITYVK_SMOKE_REPEAT_COUNT=3 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=1 \
./scripts/local_smoke.sh
```

If instability is expected during exploration, temporarily allow it:

```bash
REALITYVK_SMOKE_REPEAT_COUNT=3 \
REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE=0 \
REALITYVK_SMOKE_REQUIRE_MAJORITY=1 \
./scripts/local_smoke.sh
```

## Runner placeholders

`REALITYVK_SMOKE_RUNNER` supports:

- `{backend}`
- `{scenario}`
- `{rom}`
- `{frames}`
- `{out_file}`
- `{out_dir}`
- `{scenario_args}`
