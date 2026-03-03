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

## Key Conventions
- Runtime path policy: `rvk2` only.
- Test policy: Vulkan smoke + upstream `GLideN64` comparison only.
- Keep candidate captures deterministic: `dumpfb-preset` path.
- Keep deep telemetry centered on `paper_mario_intro`.
- Keep docs condensed: update canonical files, delete stale docs.
- Do not reintroduce non-Vulkan or non-Mupen compatibility paths.

## Deep Smoke
- Primary analysis run:
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_DEEP_TELEMETRY=1 \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_parity.sh
```
- Deep telemetry archives are mandatory by default (`REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=1`).
- Archive root: `build/parity-runs/paper-mario/archive`.
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`.
- Latest archive symlink: `build/parity-runs/paper-mario/archive/paper_mario_intro.latest`.

## Smoke Selection
- Use deep smoke when render/present behavior changed (`rvk2` raster, texrect/triangle, VI/handoff, telemetry logic).
- Use deep smoke when quick smoke indicates visual mismatch and stage attribution is needed.
- Use deep smoke when capturing before/after evidence for a fix checkpoint.
- Skip deep smoke for compile/unit/conformance-only failures with no visual-path changes.
- Skip deep smoke during rapid non-render refactors.
- Skip deep smoke for obvious logic bug iteration when quick smoke is sufficient.
- Replay performance default: `REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS=0` (all cores).
- Use `REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL=1` only for sequential state-carry investigations.

## Screenshot Expectations
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.reference.png`.
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.candidate.png`.
- Every parity run must produce `build/parity-runs/paper-mario/paper_mario_intro.compare_side_by_side.latest.png`.
- Keep `REALITYVK_PM_AUTO_COMPARE_VIEW=1`.
- Close stale viewers before open (`REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG=1`).
- Open side-by-side image in `eog`.
- Use screenshot output for monitoring only; use archived metrics + telemetry bundle for conclusions.

## Local Skills
- Use `agents-md` for `AGENTS.md` maintenance (`/home/auro/.agents/skills/agents-md/SKILL.md`).
- If a task names any available skill, open that skill's `SKILL.md` and follow it.
