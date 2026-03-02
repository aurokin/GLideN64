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
- Keep scripted captures deterministic: `dumpfb-preset` path only.
- Keep docs condensed: update canonical files, delete stale docs.
- Do not reintroduce non-Vulkan or non-Mupen compatibility paths.

## Local Skills
- Use `agents-md` for `AGENTS.md` maintenance (`/home/auro/.agents/skills/agents-md/SKILL.md`).
- Use `doc-coauthoring` when drafting or restructuring project docs (`/home/auro/.agents/skills/doc-coauthoring/SKILL.md`).
- If a task names any available skill, open that skill's `SKILL.md` and follow it.
