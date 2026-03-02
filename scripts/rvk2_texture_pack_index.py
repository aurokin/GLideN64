#!/usr/bin/env python3
"""Deterministic tooling for rkv2 texture pack indexes.

Index format (rkv2_pack_index_v1.tsv):
  hi<TAB>lo<TAB>width<TAB>height<TAB>rgba_file

All values are deterministic and sorted by (hi, lo, rgba_file).
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import sys
from pathlib import Path
from typing import Iterable


INDEX_FILENAME = "rkv2_pack_index_v1.tsv"
MAX_U64 = (1 << 64) - 1
MAX_U16 = (1 << 16) - 1
RGBA_EXT = ".rgba32"
FILENAME_RE = re.compile(
    r"^(0x[0-9A-Fa-f]+|[0-9]+)_(0x[0-9A-Fa-f]+|[0-9]+)_([1-9][0-9]*)x([1-9][0-9]*)(?:_[A-Za-z0-9._-]+)?\.rgba32$"
)


@dataclasses.dataclass(frozen=True)
class PackEntry:
    hi: int
    lo: int
    width: int
    height: int
    rgba_file: str

    @property
    def key(self) -> tuple[int, int]:
        return (self.hi, self.lo)

    @property
    def expected_size(self) -> int:
        return self.width * self.height * 4

    def canonical_row(self) -> str:
        return (
            f"0x{self.hi:016X}\t"
            f"0x{self.lo:016X}\t"
            f"{self.width}\t"
            f"{self.height}\t"
            f"{self.rgba_file}\n"
        )


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def parse_int_token(token: str, field_name: str, line_number: int | None = None) -> int:
    where = f"line {line_number}" if line_number is not None else "input"
    value_token = token.strip()
    base = 16 if value_token.lower().startswith("0x") else 10
    try:
        value = int(value_token, base)
    except ValueError as exc:
        raise ValueError(f"{where}: invalid {field_name} value '{token}'") from exc
    if value < 0:
        raise ValueError(f"{where}: {field_name} must be non-negative")
    return value


def parse_u64_token(token: str, field_name: str, line_number: int | None = None) -> int:
    value = parse_int_token(token, field_name, line_number)
    if value > MAX_U64:
        where = f"line {line_number}" if line_number is not None else "input"
        raise ValueError(f"{where}: {field_name} exceeds u64 range")
    return value


def parse_u16_token(token: str, field_name: str, line_number: int | None = None) -> int:
    value = parse_int_token(token, field_name, line_number)
    if value == 0 or value > MAX_U16:
        where = f"line {line_number}" if line_number is not None else "input"
        raise ValueError(f"{where}: {field_name} must be in range [1, 65535]")
    return value


def canonical_sort(entries: Iterable[PackEntry]) -> list[PackEntry]:
    return sorted(entries, key=lambda e: (e.hi, e.lo, e.rgba_file))


def parse_entry_from_filename(path: Path, pack_dir: Path) -> PackEntry | None:
    match = FILENAME_RE.match(path.name)
    if match is None:
        return None
    hi = parse_u64_token(match.group(1), "hi")
    lo = parse_u64_token(match.group(2), "lo")
    width = parse_u16_token(match.group(3), "width")
    height = parse_u16_token(match.group(4), "height")
    rel = path.relative_to(pack_dir).as_posix()
    return PackEntry(hi=hi, lo=lo, width=width, height=height, rgba_file=rel)


def parse_index_line(line: str, line_number: int) -> PackEntry:
    fields = [field.strip() for field in line.split("\t")]
    if len(fields) != 5:
        raise ValueError(f"line {line_number}: expected 5 tab-separated fields, got {len(fields)}")
    hi = parse_u64_token(fields[0], "hi", line_number)
    lo = parse_u64_token(fields[1], "lo", line_number)
    width = parse_u16_token(fields[2], "width", line_number)
    height = parse_u16_token(fields[3], "height", line_number)
    rgba_file = fields[4]
    if not rgba_file:
        raise ValueError(f"line {line_number}: rgba_file must be non-empty")
    return PackEntry(hi=hi, lo=lo, width=width, height=height, rgba_file=rgba_file)


def validate_entry_files(pack_dir: Path, entries: list[PackEntry], allow_absolute_paths: bool) -> list[str]:
    errors: list[str] = []
    for entry in entries:
        rgba_path = Path(entry.rgba_file)
        if rgba_path.is_absolute():
            if not allow_absolute_paths:
                errors.append(
                    f"entry (hi=0x{entry.hi:016X}, lo=0x{entry.lo:016X}) uses absolute rgba_file path"
                )
                continue
            resolved = rgba_path
        else:
            resolved = pack_dir / rgba_path
        if not resolved.exists():
            errors.append(
                f"entry (hi=0x{entry.hi:016X}, lo=0x{entry.lo:016X}) references missing file: {resolved}"
            )
            continue
        if not resolved.is_file():
            errors.append(
                f"entry (hi=0x{entry.hi:016X}, lo=0x{entry.lo:016X}) references non-file path: {resolved}"
            )
            continue
        size = resolved.stat().st_size
        if size != entry.expected_size:
            errors.append(
                "entry (hi=0x{hi:016X}, lo=0x{lo:016X}) size mismatch: file={file_size} expected={expected}".format(
                    hi=entry.hi,
                    lo=entry.lo,
                    file_size=size,
                    expected=entry.expected_size,
                )
            )
    return errors


def command_generate(args: argparse.Namespace) -> int:
    pack_dir = Path(args.pack_dir).resolve()
    if not pack_dir.is_dir():
        return fail(f"pack directory does not exist: {pack_dir}")
    index_path = Path(args.index).resolve() if args.index else (pack_dir / INDEX_FILENAME)

    files = sorted(
        (pack_dir.rglob(f"*{RGBA_EXT}") if args.recursive else pack_dir.glob(f"*{RGBA_EXT}")),
        key=lambda p: p.as_posix(),
    )
    if not files and not args.allow_empty:
        return fail(f"no {RGBA_EXT} files found in pack directory: {pack_dir}")

    entries: list[PackEntry] = []
    unmatched: list[Path] = []
    key_sources: dict[tuple[int, int], str] = {}
    for rgba_file in files:
        entry = parse_entry_from_filename(rgba_file, pack_dir)
        if entry is None:
            unmatched.append(rgba_file)
            continue
        existing = key_sources.get(entry.key)
        if existing is not None:
            return fail(
                "duplicate cache key discovered while scanning files: "
                f"0x{entry.hi:016X}/0x{entry.lo:016X} from '{existing}' and '{entry.rgba_file}'"
            )
        key_sources[entry.key] = entry.rgba_file
        actual_size = rgba_file.stat().st_size
        if actual_size != entry.expected_size:
            return fail(
                "rgba file size mismatch for '{name}': file={file_size} expected={expected}".format(
                    name=entry.rgba_file,
                    file_size=actual_size,
                    expected=entry.expected_size,
                )
            )
        entries.append(entry)

    if unmatched and not args.allow_unmatched:
        sample = "\n".join(f"  - {path.relative_to(pack_dir).as_posix()}" for path in unmatched[:20])
        return fail(
            f"found {len(unmatched)} rgba files that do not match naming convention "
            "<hi>_<lo>_<width>x<height>[...].rgba32:\n{sample}"
        )

    if not entries and not args.allow_empty:
        return fail("no indexable rgba files found (all candidates were unmatched)")

    sorted_entries = canonical_sort(entries)
    index_path.parent.mkdir(parents=True, exist_ok=True)
    with index_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# rkv2_pack_index_v1\n")
        handle.write("# columns: hi\tlo\twidth\theight\trgba_file\n")
        for entry in sorted_entries:
            handle.write(entry.canonical_row())

    if not args.quiet:
        total_pixels = sum(entry.width * entry.height for entry in sorted_entries)
        print(
            "Generated index: {path} (entries={entries}, pixels={pixels}, unmatched={unmatched})".format(
                path=index_path,
                entries=len(sorted_entries),
                pixels=total_pixels,
                unmatched=len(unmatched),
            )
        )
    return 0


def command_validate(args: argparse.Namespace) -> int:
    pack_dir = Path(args.pack_dir).resolve()
    if not pack_dir.is_dir():
        return fail(f"pack directory does not exist: {pack_dir}")
    index_path = Path(args.index).resolve() if args.index else (pack_dir / INDEX_FILENAME)
    if not index_path.is_file():
        return fail(f"index file does not exist: {index_path}")

    entries: list[PackEntry] = []
    parse_errors: list[str] = []
    with index_path.open("r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                entry = parse_index_line(line, line_number)
            except ValueError as exc:
                parse_errors.append(str(exc))
                continue
            entries.append(entry)

    if parse_errors:
        for error in parse_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if not entries and not args.allow_empty:
        return fail("index has no entries")

    duplicate_errors: list[str] = []
    seen_keys: dict[tuple[int, int], str] = {}
    for entry in entries:
        previous = seen_keys.get(entry.key)
        if previous is not None:
            duplicate_errors.append(
                "duplicate key in index: 0x{hi:016X}/0x{lo:016X} -> '{prev}' and '{curr}'".format(
                    hi=entry.hi,
                    lo=entry.lo,
                    prev=previous,
                    curr=entry.rgba_file,
                )
            )
        else:
            seen_keys[entry.key] = entry.rgba_file
    if duplicate_errors:
        for error in duplicate_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    file_errors = validate_entry_files(pack_dir, entries, args.allow_absolute_paths)
    if file_errors:
        for error in file_errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    sorted_entries = canonical_sort(entries)
    is_canonical_order = entries == sorted_entries
    if not is_canonical_order and not args.rewrite:
        return fail("index is not in canonical order; rerun with --rewrite")

    if args.require_index_covers_pack:
        indexed_paths = {
            Path(entry.rgba_file).as_posix()
            for entry in entries
            if not Path(entry.rgba_file).is_absolute()
        }
        missing: list[str] = []
        for rgba in sorted(pack_dir.rglob(f"*{RGBA_EXT}"), key=lambda p: p.as_posix()):
            rel = rgba.relative_to(pack_dir).as_posix()
            if rel not in indexed_paths:
                missing.append(rel)
        if missing:
            sample = "\n".join(f"  - {rel}" for rel in missing[:20])
            return fail(
                f"pack contains {len(missing)} unindexed {RGBA_EXT} files:\n{sample}"
            )

    if args.rewrite:
        with index_path.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("# rkv2_pack_index_v1\n")
            handle.write("# columns: hi\tlo\twidth\theight\trgba_file\n")
            for entry in sorted_entries:
                handle.write(entry.canonical_row())

    if not args.quiet:
        total_pixels = sum(entry.width * entry.height for entry in sorted_entries)
        print(
            "Validated index: {path} (entries={entries}, pixels={pixels}, canonical={canonical})".format(
                path=index_path,
                entries=len(sorted_entries),
                pixels=total_pixels,
                canonical="yes" if is_canonical_order else "rewritten",
            )
        )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate/validate deterministic rkv2 texture pack index files."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    gen = subparsers.add_parser("generate", help="Generate rkv2_pack_index_v1.tsv from rgba file names.")
    gen.add_argument("--pack-dir", required=True, help="Texture pack directory.")
    gen.add_argument("--index", default="", help="Output index path (default: <pack-dir>/rkv2_pack_index_v1.tsv).")
    gen.add_argument("--recursive", action="store_true", default=True, help="Recursively scan pack directory (default: true).")
    gen.add_argument("--no-recursive", action="store_false", dest="recursive", help="Scan only top-level pack directory.")
    gen.add_argument("--allow-unmatched", action="store_true", help="Allow .rgba32 files that do not match naming convention.")
    gen.add_argument("--allow-empty", action="store_true", help="Allow writing an empty index.")
    gen.add_argument("--quiet", action="store_true", help="Suppress summary output.")
    gen.set_defaults(func=command_generate)

    val = subparsers.add_parser("validate", help="Validate an existing rkv2_pack_index_v1.tsv.")
    val.add_argument("--pack-dir", required=True, help="Texture pack directory.")
    val.add_argument("--index", default="", help="Index path (default: <pack-dir>/rkv2_pack_index_v1.tsv).")
    val.add_argument("--rewrite", action="store_true", help="Rewrite index into canonical sorted order.")
    val.add_argument("--allow-empty", action="store_true", help="Allow empty index.")
    val.add_argument(
        "--allow-absolute-paths",
        action="store_true",
        help="Allow absolute rgba_file paths in index rows.",
    )
    val.add_argument(
        "--require-index-covers-pack",
        action="store_true",
        help=f"Fail if any {RGBA_EXT} file under pack-dir is not indexed.",
    )
    val.add_argument("--quiet", action="store_true", help="Suppress summary output.")
    val.set_defaults(func=command_validate)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
