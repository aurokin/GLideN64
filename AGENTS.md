# Agent Instructions

## Package Manager
- Use system tools only: `cmake`, `python3`, `bash`.
- Configure/build: `cmake -S src -B build/release-vulkan-smoke -DCMAKE_BUILD_TYPE=Release` then `cmake --build build/release-vulkan-smoke -j$(nproc)`.
- Required gate: `./scripts/local_gate.sh`.

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

## Local Skills
- Use `agents-md` for `AGENTS.md` maintenance (`/home/auro/.agents/skills/agents-md/SKILL.md`).
- If a task names any available skill, open that skill's `SKILL.md` and follow it.
