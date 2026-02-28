#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is required but was not found in PATH." >&2
  exit 127
fi

git -C "${ROOT_DIR}" config core.hooksPath .githooks
chmod +x "${ROOT_DIR}/.githooks/pre-push"

echo "Configured local git hooks path: .githooks"
echo "pre-push hook is now active and will run ./scripts/local_gate.sh"
