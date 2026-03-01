#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERT_SRC="$ROOT_DIR/src/Graphics/VulkanContext/shaders/basic_textured.vert"
FRAG_SRC="$ROOT_DIR/src/Graphics/VulkanContext/shaders/basic_textured.frag"
OUT_HEADER="$ROOT_DIR/src/Graphics/VulkanContext/vulkan_BasicTexturedShaders.h"

if ! command -v glslangValidator >/dev/null 2>&1; then
  echo "glslangValidator is required but was not found in PATH." >&2
  exit 1
fi

if ! command -v xxd >/dev/null 2>&1; then
  echo "xxd is required but was not found in PATH." >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

VERT_SPV="$TMP_DIR/basic_textured.vert.spv"
FRAG_SPV="$TMP_DIR/basic_textured.frag.spv"

glslangValidator -V "$VERT_SRC" -o "$VERT_SPV" >/dev/null
glslangValidator -V "$FRAG_SRC" -o "$FRAG_SPV" >/dev/null

xxd -i "$VERT_SPV" > "$TMP_DIR/vert_raw.h"
xxd -i "$FRAG_SPV" > "$TMP_DIR/frag_raw.h"

perl -pe 's/^unsigned char [^\[]+\[\]/static const unsigned char kBasicTexturedVertSpv[]/; s/^unsigned int [^=]+/static const unsigned int kBasicTexturedVertSpv_len/' \
  "$TMP_DIR/vert_raw.h" > "$TMP_DIR/vert.h"
perl -pe 's/^unsigned char [^\[]+\[\]/static const unsigned char kBasicTexturedFragSpv[]/; s/^unsigned int [^=]+/static const unsigned int kBasicTexturedFragSpv_len/' \
  "$TMP_DIR/frag_raw.h" > "$TMP_DIR/frag.h"

{
  echo "#pragma once"
  echo
  cat "$TMP_DIR/vert.h"
  echo
  cat "$TMP_DIR/frag.h"
} > "$OUT_HEADER"

echo "Regenerated $OUT_HEADER"
