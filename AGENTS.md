# Agent Instructions

## Package Manager
- Use system tools only: `cmake`, `python3`, `bash`.
- Configure/build: `cmake -S src -B build/release-vulkan-smoke -DCMAKE_BUILD_TYPE=Release` then `cmake --build build/release-vulkan-smoke -j$(nproc)`.
- Required gate: `./scripts/local_gate.sh`.

## Commit Attribution
- AI commits MUST include:
```text
Co-Authored-By: Codex GPT-5 <codex@openai.com>
```

## Mission (Must-Know)
- Objective: fix missing textures and geometry in `paper_mario_intro` on Vulkan `rvk2`.
- Acceptance bar (summary):
  - `candidate_non_black_ratio >= 0.90`
  - `missing_without_write_ratio <= 0.35`
  - `rmse <= 0.25`
  - `mae <= 0.20`
- Canonical runbook: `docs/workflow.md`.
- Canonical priority queue: `docs/status.md`.

## Key Conventions
- Runtime path policy: `rvk2` only.
- Test policy: Vulkan smoke + upstream `GLideN64` comparison only.
- Keep candidate captures deterministic: `dumpfb-preset` path.
- Reference `GLideN64` in agent-mode `dumpfb-preset` is expected black; parity script auto-bypasses reference non-black validation for that path.
- Keep deep telemetry centered on `paper_mario_intro`.
- Deep profile preflight requires canonical telemetry keys in candidate plugin (`REALITYVK_RVK2_TRACE_FILE`, `REALITYVK_RVK2_PACKET_TRACE_FILE`, `REALITYVK_RVK2_FRAME_FORENSICS_FILE`); rebuild plugin if stale.
- Deep profile prunes non-archived telemetry clutter by default (`REALITYVK_PM_TELEMETRY_PRUNE_ENABLE=1`).
- Local-only workflow is expected for now; hardcoded local path defaults are not treated as a blocker.
- Keep docs condensed: update canonical files, delete stale docs.
- Do not reintroduce non-Vulkan or non-Mupen compatibility paths.

## Local Environment Assumptions
- Candidate plugin default: `build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so`.
- Candidate core default: `/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2`.
- Reference plugin default: `/home/auro/code/gliden64-upstream/build-release/plugin/Release/mupen64plus-video-GLideN64.so`.
- Reference core default: `/home/auro/code/mupen/mupen64plus-core-upstream/projects/unix/libmupen64plus.so.2`.
- Override paths with `REALITYVK_PM_*` env vars when needed.

## Smoke Policy
- Quick smoke (default for fast iteration):
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=basic \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_parity.sh
```
- Deep smoke (required for render/present behavior changes and checkpoint evidence):
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=deep \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_parity.sh
```
- Use deep smoke when raster/texrect/triangle/VI/handoff/telemetry behavior changes.
- Skip deep smoke for compile/unit/conformance-only failures or non-render refactors.
- Replay performance default: `REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS=0` (all cores).
- Use `REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL=1` only for sequential state-carry checks.

## Archive Comparison Workflow
- Archive defaults are profile-driven:
  - `basic`: `REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=0`
  - `deep`: `REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=1`
- Archive root: `build/parity-runs/paper-mario/archive`.
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`.
- Latest archive symlink: `build/parity-runs/paper-mario/archive/paper_mario_intro.latest`.
- Compare the latest deep run against the prior deep run:
```bash
python3 scripts/rvk2_archive_compare.py \
  --index build/parity-runs/paper-mario/archive/index.tsv \
  --scenario paper_mario_intro
```
- For durable reports, emit artifacts:
```bash
python3 scripts/rvk2_archive_compare.py \
  --index build/parity-runs/paper-mario/archive/index.tsv \
  --scenario paper_mario_intro \
  --json-out build/parity-runs/paper-mario/archive/compare.latest.json \
  --md-out build/parity-runs/paper-mario/archive/compare.latest.md
```
- Minimum comparison fields: `rmse`, `mae`, `candidate_non_black_ratio`, `candidate_mean_luma`, `missing_without_write_ratio`, telemetry bundle `suspected_gaps`.
- Use archived evidence for conclusions; do not rely only on on-screen screenshots.
- Manual telemetry prune command:
```bash
./scripts/paper_mario_telemetry_cleanup.sh
```

## Knob History Workflow
- History file: `build/parity-runs/paper-mario/knob-history.tsv`.
- Default tracking is on (`REALITYVK_PM_KNOB_TRACK_ENABLE=1`).
- Summarize recent runs:
```bash
python3 scripts/rvk2_knob_history.py summary \
  --history build/parity-runs/paper-mario/knob-history.tsv \
  --scenario-id paper_mario_intro \
  --limit 12
```

## Replay Interpretation Rule
- Non-stateful replay can report broad frame failures; treat it as signal, not final class attribution.
- For deterministic first-divergence classification, use stateful replay (`--stateful-frames`, jobs `1`).
- Prioritize bundle fields tied to missing-region attribution and present/handoff evidence over raw replay failure count.

## Debug Toggle Safety Policy
- `REALITYVK_RVK2_DEBUG_*` toggles are probe-only unless explicitly promoted.
- Runtime emits a one-time active-toggle summary when non-default `REALITYVK_RVK2_DEBUG_*` values are detected (`REALITYVK_RVK2_DEBUG_LOG_ACTIVE=0` disables summary logging).
- Do not land behavior fixes that require permanent debug toggles.
- Revert temporary source probes before merge unless converted into intentional telemetry with docs.
- Keep release-path defaults stable; drive experiments through smoke env vars.

## Screenshot Expectations
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.reference.png`.
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.candidate.png`.
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.compare_side_by_side.latest.png`.
- Keep `REALITYVK_PM_AUTO_COMPARE_VIEW=1`.
- Close stale viewers before open (`REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG=1`).
- Open side-by-side image in `eog`.
- Use screenshot output for monitoring only; use archived metrics + telemetry bundle for conclusions.

## Collaboration Policy
- Commit and push incremental checkpoints during active debugging.
- Keep commits scoped (single lane or single telemetry change-set).
- If blocked by missing external data/access, stop and report the blocker explicitly.
- Include required co-author attribution on every AI commit.

## Local Skills
- Use `agents-md` for `AGENTS.md` maintenance (`/home/auro/.agents/skills/agents-md/SKILL.md`).
- If a task names any available skill, open that skill's `SKILL.md` and follow it.
