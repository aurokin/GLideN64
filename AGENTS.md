# Repository Guidelines

## Project Structure & Module Organization
- `src/`: core plugin source.
- `src/Graphics/VulkanContext/`: Vulkan backend implementation (primary render path).
- `src/mupenplus/`: Mupen64Plus plugin integration.
- `src/uCodes/`: N64 microcode decoders/execution paths.
- `scripts/`: local build/test/parity automation.
- `tests/smoke/`: deterministic smoke scenarios and checksum baselines.
- `docs/`: workflow, migration plan, smoke/local CI notes.
- `build/`: generated artifacts only; do not commit generated outputs.

## Build, Test, and Development Commands
```bash
# Configure + build (Linux CLI plugin)
cmake -S src -B build/release-vulkan-smoke -DCMAKE_BUILD_TYPE=Release -DMUPENPLUSAPI=ON -DMUPENPLUSAPI_GLIDENUI=OFF
cmake --build build/release-vulkan-smoke -j$(nproc)

# Required local CI gate (CI is disabled in this fork)
./scripts/local_gate.sh

# Gate + Paper Mario parity smoke
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh

# Direct parity and smoke utilities
./scripts/paper_mario_parity.sh
./scripts/local_smoke.sh
./scripts/paper_mario_compare_view.sh
```

## Coding Style & Naming Conventions
- Language: C++17 for plugin code; Bash/Python for tooling.
- Match surrounding style in touched files; do not restyle unrelated code.
- C++ style in this repo is legacy-leaning: tabs are common, braces on their own lines, clear `PascalCase` types and `camelCase` methods.
- Shell scripts should keep `set -euo pipefail` and use uppercase env vars (example: `REALITYVK_*`).
- Prefer descriptive feature flags over hardcoded debug behavior.

## Testing Guidelines
- Primary gate is deterministic smoke/parity, not broad unit tests.
- Keep Paper Mario parity working (`tests/smoke/scenarios.tsv`, `scripts/paper_mario_parity.sh`).
- Baselines live in `tests/smoke/baselines/*.checksums.tsv`; update only for intentional rendering changes.
- When changing render behavior, include before/after artifacts under `build/parity-runs/paper-mario/` during review.

## Commit & Pull Request Guidelines
- Use concise, imperative commit messages; common prefixes in history: `feat:`, `fix:`, `refactor:`, `chore:`.
- Keep commits focused (build system, renderer behavior, tooling, docs separated when practical).
- PRs should include:
- scope and rationale,
- exact validation commands run,
- Paper Mario parity outcome (metrics/artifact paths),
- linked issue(s) when applicable.
- This branch is Vulkan-first: avoid reintroducing OpenGL compatibility paths unless explicitly requested.
