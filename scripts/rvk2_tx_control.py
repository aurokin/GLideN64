#!/usr/bin/env python3
"""Manage RealityVK2 texture replacement runtime control files."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict


CANONICAL_KEYS = [
    "enable",
    "cache_path",
    "pack_path",
    "max_entries",
    "max_pixels",
    "reload_token",
    "invalidate_token",
    "log_summary",
    "summary_path",
]

DEFAULTS: Dict[str, str] = {
    "enable": "1",
    "cache_path": "",
    "pack_path": "",
    "max_entries": "0",
    "max_pixels": "0",
    "reload_token": "0",
    "invalidate_token": "0",
    "log_summary": "0",
    "summary_path": "",
}


def load_control(path: Path) -> Dict[str, str]:
    data = dict(DEFAULTS)
    if not path.exists():
        return data
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        separator = "=" if "=" in line else "\t"
        if separator not in line:
            continue
        key_part, value_part = line.split(separator, 1)
        key = key_part.strip().lower()
        if key not in DEFAULTS:
            continue
        data[key] = value_part.strip()
    return data


def save_control(path: Path, data: Dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{key}={data.get(key, DEFAULTS[key])}" for key in CANONICAL_KEYS]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_u64(value: str, field_name: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise ValueError(f"invalid {field_name}: '{value}'") from exc
    if parsed < 0:
        raise ValueError(f"invalid {field_name}: must be >= 0")
    if parsed > 0xFFFFFFFFFFFFFFFF:
        raise ValueError(f"invalid {field_name}: exceeds u64")
    return parsed


def parse_bool01(value: str, field_name: str) -> str:
    if value not in ("0", "1"):
        raise ValueError(f"invalid {field_name}: expected 0 or 1")
    return value


def cmd_show(args: argparse.Namespace) -> int:
    data = load_control(Path(args.control_file))
    for key in CANONICAL_KEYS:
        print(f"{key}={data[key]}")
    return 0


def cmd_set(args: argparse.Namespace) -> int:
    path = Path(args.control_file)
    data = load_control(path)
    if args.enable is not None:
        data["enable"] = parse_bool01(args.enable, "enable")
    if args.cache_path is not None:
        data["cache_path"] = args.cache_path
    if args.pack_path is not None:
        data["pack_path"] = args.pack_path
    if args.max_entries is not None:
        data["max_entries"] = str(parse_u64(args.max_entries, "max_entries"))
    if args.max_pixels is not None:
        data["max_pixels"] = str(parse_u64(args.max_pixels, "max_pixels"))
    if args.log_summary is not None:
        data["log_summary"] = parse_bool01(args.log_summary, "log_summary")
    if args.summary_path is not None:
        data["summary_path"] = args.summary_path
    save_control(path, data)
    return 0


def cmd_bump_reload(args: argparse.Namespace) -> int:
    path = Path(args.control_file)
    data = load_control(path)
    reload_token = parse_u64(data.get("reload_token", "0"), "reload_token")
    data["reload_token"] = str(reload_token + 1)
    save_control(path, data)
    return 0


def cmd_bump_invalidate(args: argparse.Namespace) -> int:
    path = Path(args.control_file)
    data = load_control(path)
    invalidate_token = parse_u64(data.get("invalidate_token", "0"), "invalidate_token")
    data["invalidate_token"] = str(invalidate_token + 1)
    save_control(path, data)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Manage RealityVK2 texture replacement runtime control files."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="Print the current control file values.")
    show.add_argument("--control-file", required=True, help="Path to control file.")
    show.set_defaults(func=cmd_show)

    set_cmd = sub.add_parser("set", help="Set one or more control file values.")
    set_cmd.add_argument("--control-file", required=True, help="Path to control file.")
    set_cmd.add_argument("--enable", choices=("0", "1"), help="Enable texture replacement.")
    set_cmd.add_argument("--cache-path", help="Path to .hts cache file.")
    set_cmd.add_argument("--pack-path", help="Path to texture pack directory.")
    set_cmd.add_argument("--max-entries", help="Maximum replacement entry count.")
    set_cmd.add_argument("--max-pixels", help="Maximum replacement pixel count.")
    set_cmd.add_argument(
        "--log-summary",
        choices=("0", "1"),
        help="Enable per-frame log summary output.",
    )
    set_cmd.add_argument(
        "--summary-path",
        help="Write per-frame replacement summary to this file.",
    )
    set_cmd.set_defaults(func=cmd_set)

    bump_reload = sub.add_parser(
        "bump-reload",
        help="Increment reload token to force replacement cache/pack reload.",
    )
    bump_reload.add_argument("--control-file", required=True, help="Path to control file.")
    bump_reload.set_defaults(func=cmd_bump_reload)

    bump_invalidate = sub.add_parser(
        "bump-invalidate",
        help="Increment invalidate token to force replacement state reset.",
    )
    bump_invalidate.add_argument("--control-file", required=True, help="Path to control file.")
    bump_invalidate.set_defaults(func=cmd_bump_invalidate)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return int(args.func(args))
    except ValueError as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
