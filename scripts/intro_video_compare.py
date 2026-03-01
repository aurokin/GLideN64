#!/usr/bin/env python3
"""Capture and compare N64 intro videos across reference and candidate plugins.

This script uses the local paper_mario agent runtime to:
1. Boot selected ROMs.
2. Capture deterministic frame sequences for reference and candidate plugin runs.
3. Render comparison videos (reference, candidate, side-by-side, diff, triptych).
4. Compute frame-diff metrics and emit per-game + aggregate reports.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image


SUPPORTED_ROM_EXTS = (".z64", ".n64", ".v64", ".bin", ".rom")


@dataclass
class GameEntry:
    game_id: str
    rom_pattern: str
    rationale: str


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run_cmd(
    cmd: Sequence[str],
    *,
    env: Optional[Dict[str, str]] = None,
    timeout_sec: Optional[float] = None,
    check: bool = True,
    quiet: bool = False,
) -> subprocess.CompletedProcess[str]:
    kwargs = {
        "text": True,
        "env": env,
    }
    if quiet:
        kwargs["stdout"] = subprocess.DEVNULL
        kwargs["stderr"] = subprocess.DEVNULL
    else:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    proc = subprocess.run(cmd, timeout=timeout_sec, **kwargs)
    if check and proc.returncode != 0:
        stderr = (proc.stderr or "").strip()
        stdout = (proc.stdout or "").strip()
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\nstdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return proc


def load_manifest(path: Path) -> List[GameEntry]:
    entries: List[GameEntry] = []
    with path.open("r", encoding="utf-8") as f:
        reader = csv.reader(f, delimiter="\t")
        for row in reader:
            if not row:
                continue
            if row[0].strip().startswith("#"):
                continue
            if len(row) < 2:
                continue
            game_id = row[0].strip()
            rom_pattern = row[1].strip()
            rationale = row[2].strip() if len(row) >= 3 else ""
            if not game_id or not rom_pattern:
                continue
            entries.append(GameEntry(game_id=game_id, rom_pattern=rom_pattern, rationale=rationale))
    return entries


def resolve_rom(rom_root: Path, pattern: str) -> Path:
    # Absolute path takes precedence.
    candidate = Path(pattern).expanduser()
    if candidate.is_absolute() and candidate.exists():
        return candidate

    matches = sorted(rom_root.glob(pattern))
    if not matches:
        raise FileNotFoundError(f"no ROM matched pattern '{pattern}' under {rom_root}")
    return matches[0]


def materialize_rom(rom_path: Path, work_dir: Path) -> Tuple[Path, Optional[Path]]:
    """Return (run_rom_path, extracted_dir_if_any)."""
    if rom_path.suffix.lower() != ".zip":
        return rom_path, None

    extract_root = work_dir / "rom_extract"
    extract_root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(rom_path, "r") as zf:
        names = [n for n in zf.namelist() if Path(n).suffix.lower() in SUPPORTED_ROM_EXTS]
        if not names:
            raise RuntimeError(f"zip has no supported ROM file: {rom_path}")
        # Prefer common native order.
        names.sort(key=lambda n: (Path(n).suffix.lower(), n))
        chosen = names[0]
        out_path = extract_root / Path(chosen).name
        with zf.open(chosen) as src, out_path.open("wb") as dst:
            shutil.copyfileobj(src, dst)
    return out_path, extract_root


def wait_for_socket(socket_path: Path, proc: subprocess.Popen[str], timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if socket_path.exists():
            return
        if proc.poll() is not None:
            raise RuntimeError("emulator exited before agent socket became ready")
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for agent socket: {socket_path}")


def terminate_process(proc: Optional[subprocess.Popen[str]], grace_sec: float = 4.0) -> None:
    if proc is None:
        return
    if proc.poll() is not None:
        return
    try:
        proc.terminate()
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace_sec
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return
        time.sleep(0.05)
    try:
        proc.kill()
    except ProcessLookupError:
        pass


def run_agentctl(
    agentctl_path: Path,
    socket_path: Path,
    args: Sequence[str],
    *,
    timeout_sec: float,
    quiet: bool,
) -> None:
    cmd = ["python3", str(agentctl_path), "--socket", str(socket_path), *args]
    run_cmd(cmd, timeout_sec=timeout_sec, check=True, quiet=quiet)


def capture_backend_sequence(
    *,
    game_id: str,
    backend_label: str,
    runtime_backend: str,
    rom_run_path: Path,
    plugin_path: Path,
    frames_dir: Path,
    captures: int,
    step_frames: int,
    scale_div: int,
    runtime_root: Path,
    launch_timeout_sec: float,
    command_timeout_sec: float,
    launch_quiet: bool,
    command_quiet: bool,
    agent_retries: int,
    recovery_sleep_sec: float,
    warmup_frames: int,
) -> Dict[str, object]:
    runtime_dir = runtime_root / "mupen64plus-runtime"
    launch_script = runtime_dir / "launch.sh"
    agentctl = runtime_dir / "agentctl.py"
    plugin_dest = runtime_dir / "plugins" / "mupen64plus-video-RealityVK.so"

    if not launch_script.exists():
        raise FileNotFoundError(f"launch script not found: {launch_script}")
    if not agentctl.exists():
        raise FileNotFoundError(f"agentctl not found: {agentctl}")
    if not plugin_path.exists():
        raise FileNotFoundError(f"plugin not found: {plugin_path}")

    frames_dir.mkdir(parents=True, exist_ok=True)
    plugin_dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(plugin_path, plugin_dest)
    plugin_dest.chmod(0o755)

    socket_path = Path(f"/tmp/m64-agent-intro-{game_id}-{backend_label}-{os.getpid()}.sock")
    if socket_path.exists():
        socket_path.unlink()

    launch_cmd = [
        str(launch_script),
        str(rom_run_path),
        "--agent-server",
        str(socket_path),
        "--agent-profile",
        "watch",
        "--gfx",
        plugin_dest.name,
    ]
    launch_env = os.environ.copy()
    launch_env["REALITYVK_GRAPHICS_BACKEND"] = runtime_backend

    proc: Optional[subprocess.Popen[str]] = None
    frame_paths: List[Path] = []

    def call_agent(
        args: Sequence[str],
        *,
        recover_to_paused: bool,
    ) -> None:
        last_error: Optional[Exception] = None
        total_tries = max(1, agent_retries + 1)
        for attempt in range(total_tries):
            try:
                run_agentctl(
                    agentctl,
                    socket_path,
                    args,
                    timeout_sec=command_timeout_sec,
                    quiet=command_quiet,
                )
                return
            except Exception as exc:  # pylint: disable=broad-except
                last_error = exc
                if attempt + 1 >= total_tries:
                    break
                # Best-effort recovery to a known paused state.
                try:
                    run_agentctl(
                        agentctl,
                        socket_path,
                        ["--wait-running", "--wait-timeout", str(int(launch_timeout_sec)), "status"],
                        timeout_sec=command_timeout_sec,
                        quiet=True,
                    )
                except Exception:
                    pass
                if recover_to_paused:
                    try:
                        run_agentctl(
                            agentctl,
                            socket_path,
                            ["pause"],
                            timeout_sec=command_timeout_sec,
                            quiet=True,
                        )
                    except Exception:
                        pass
                    try:
                        run_agentctl(
                            agentctl,
                            socket_path,
                            ["--wait-state", "paused", "status"],
                            timeout_sec=command_timeout_sec,
                            quiet=True,
                        )
                    except Exception:
                        pass
                time.sleep(max(0.0, recovery_sleep_sec))
        if last_error is not None:
            raise last_error
        raise RuntimeError("agent command failed with no captured exception")

    try:
        if launch_quiet:
            proc = subprocess.Popen(
                launch_cmd,
                env=launch_env,
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        else:
            proc = subprocess.Popen(launch_cmd, env=launch_env, text=True)

        wait_for_socket(socket_path, proc, launch_timeout_sec)
        call_agent(
            ["--wait-running", "--wait-timeout", str(int(launch_timeout_sec)), "status"],
            recover_to_paused=False,
        )
        call_agent(["pause"], recover_to_paused=True)
        call_agent(["--wait-state", "paused", "status"], recover_to_paused=True)

        if warmup_frames > 0:
            call_agent(["step", str(warmup_frames)], recover_to_paused=True)

        for idx in range(captures):
            call_agent(["step", str(step_frames)], recover_to_paused=True)
            frame_path = frames_dir / f"frame_{idx:05d}.ppm"
            call_agent(
                ["dumpfb-preset", str(frame_path), "full", "--scale-div", str(scale_div)],
                recover_to_paused=True,
            )
            if not frame_path.exists() or frame_path.stat().st_size == 0:
                raise RuntimeError(f"capture frame missing or empty: {frame_path}")
            frame_paths.append(frame_path)

        call_agent(["shutdown"], recover_to_paused=False)
        if proc is not None:
            proc.wait(timeout=10.0)
    finally:
        terminate_process(proc)
        if socket_path.exists():
            try:
                socket_path.unlink()
            except OSError:
                pass

    return {
        "backend": backend_label,
        "runtime_backend": runtime_backend,
        "frames_dir": str(frames_dir),
        "frame_count": len(frame_paths),
        "frame_sha256_head": [sha256_file(p) for p in frame_paths[:5]],
        "warmup_frames": warmup_frames,
    }


def frame_diff_metrics(reference_frame: Path, candidate_frame: Path) -> Dict[str, float]:
    a = np.asarray(Image.open(reference_frame).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(candidate_frame).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        raise RuntimeError(f"shape mismatch: {reference_frame} {a.shape} vs {candidate_frame} {b.shape}")
    diff = np.abs(a - b)
    changed_mask = np.any(diff != 0, axis=2)
    return {
        "pixels_total": int(changed_mask.size),
        "pixels_diff": int(changed_mask.sum()),
        "mean_abs_diff": float(diff.mean()),
        "max_abs_diff": int(diff.max()),
    }


def build_videos(
    ffmpeg_bin: str,
    fps: int,
    reference_pattern: Path,
    candidate_pattern: Path,
    out_dir: Path,
) -> Dict[str, str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    reference_video = out_dir / "reference.mp4"
    candidate_video = out_dir / "candidate.mp4"
    sbs_video = out_dir / "side_by_side.mp4"
    diff_video = out_dir / "diff.mp4"
    triptych_video = out_dir / "triptych.mp4"

    run_cmd(
        [
            ffmpeg_bin,
            "-y",
            "-framerate",
            str(fps),
            "-i",
            str(reference_pattern),
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(reference_video),
        ],
        check=True,
        quiet=True,
    )
    run_cmd(
        [
            ffmpeg_bin,
            "-y",
            "-framerate",
            str(fps),
            "-i",
            str(candidate_pattern),
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(candidate_video),
        ],
        check=True,
        quiet=True,
    )
    run_cmd(
        [
            ffmpeg_bin,
            "-y",
            "-framerate",
            str(fps),
            "-i",
            str(reference_pattern),
            "-framerate",
            str(fps),
            "-i",
            str(candidate_pattern),
            "-filter_complex",
            "[0:v][1:v]hstack=inputs=2",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(sbs_video),
        ],
        check=True,
        quiet=True,
    )
    run_cmd(
        [
            ffmpeg_bin,
            "-y",
            "-framerate",
            str(fps),
            "-i",
            str(reference_pattern),
            "-framerate",
            str(fps),
            "-i",
            str(candidate_pattern),
            "-filter_complex",
            "[0:v][1:v]blend=all_mode=difference",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(diff_video),
        ],
        check=True,
        quiet=True,
    )
    run_cmd(
        [
            ffmpeg_bin,
            "-y",
            "-framerate",
            str(fps),
            "-i",
            str(reference_pattern),
            "-framerate",
            str(fps),
            "-i",
            str(candidate_pattern),
            "-filter_complex",
            "[0:v][1:v]blend=all_mode=difference[diff];[0:v][1:v][diff]hstack=inputs=3",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            str(triptych_video),
        ],
        check=True,
        quiet=True,
    )

    return {
        "reference_video": str(reference_video),
        "candidate_video": str(candidate_video),
        "side_by_side_video": str(sbs_video),
        "diff_video": str(diff_video),
        "triptych_video": str(triptych_video),
    }


def compare_game(
    *,
    game: GameEntry,
    rom_path: Path,
    out_dir: Path,
    reference_plugin_path: Path,
    candidate_plugin_path: Path,
    runtime_root: Path,
    captures: int,
    step_frames: int,
    scale_div: int,
    fps: int,
    ffmpeg_bin: str,
    launch_timeout_sec: float,
    command_timeout_sec: float,
    launch_quiet: bool,
    command_quiet: bool,
    agent_retries: int,
    recovery_sleep_sec: float,
    warmup_frames: int,
) -> Dict[str, object]:
    game_dir = out_dir / game.game_id
    game_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=f"intro-rom-{game.game_id}-", dir=str(game_dir)) as td:
        temp_work = Path(td)
        rom_run_path, extracted_dir = materialize_rom(rom_path, temp_work)

        captures_out: Dict[str, Dict[str, object]] = {}
        for backend_label, plugin_path in (
            ("Reference", reference_plugin_path),
            ("Candidate", candidate_plugin_path),
        ):
            backend_dir = game_dir / backend_label
            frames_dir = backend_dir / "frames"
            capture_meta = capture_backend_sequence(
                game_id=game.game_id,
                backend_label=backend_label,
                runtime_backend="Vulkan",
                rom_run_path=rom_run_path,
                plugin_path=plugin_path,
                frames_dir=frames_dir,
                captures=captures,
                step_frames=step_frames,
                scale_div=scale_div,
                runtime_root=runtime_root,
                launch_timeout_sec=launch_timeout_sec,
                command_timeout_sec=command_timeout_sec,
                launch_quiet=launch_quiet,
                command_quiet=command_quiet,
                agent_retries=agent_retries,
                recovery_sleep_sec=recovery_sleep_sec,
                warmup_frames=warmup_frames,
            )
            captures_out[backend_label] = capture_meta

        reference_dir = game_dir / "Reference" / "frames"
        candidate_dir = game_dir / "Candidate" / "frames"
        reference_frames = sorted(reference_dir.glob("frame_*.ppm"))
        candidate_frames = sorted(candidate_dir.glob("frame_*.ppm"))
        pair_count = min(len(reference_frames), len(candidate_frames))
        if pair_count == 0:
            raise RuntimeError("no paired frames captured")

        per_frame: List[Dict[str, object]] = []
        for idx in range(pair_count):
            metrics = frame_diff_metrics(reference_frames[idx], candidate_frames[idx])
            per_frame.append({"frame": idx, **metrics})

        total_pixels = int(sum(it["pixels_total"] for it in per_frame))
        total_diff = int(sum(it["pixels_diff"] for it in per_frame))
        mean_abs = float(np.mean([it["mean_abs_diff"] for it in per_frame]))
        worst = max(per_frame, key=lambda it: float(it["mean_abs_diff"]))
        changed_frames = int(sum(1 for it in per_frame if int(it["pixels_diff"]) > 0))
        identical_frames = int(pair_count - changed_frames)

        videos = build_videos(
            ffmpeg_bin=ffmpeg_bin,
            fps=fps,
            reference_pattern=reference_dir / "frame_%05d.ppm",
            candidate_pattern=candidate_dir / "frame_%05d.ppm",
            out_dir=game_dir / "videos",
        )

        result: Dict[str, object] = {
            "game_id": game.game_id,
            "rom_source": str(rom_path),
            "rom_runtime": str(rom_run_path),
            "rationale": game.rationale,
            "captures": captures_out,
            "paired_frames": pair_count,
            "identical_frames": identical_frames,
            "changed_frames": changed_frames,
            "changed_frame_ratio": float(changed_frames / pair_count),
            "pixels_total_all_frames": total_pixels,
            "pixels_diff_all_frames": total_diff,
            "pixel_diff_ratio_all_frames": float(total_diff / total_pixels) if total_pixels else 0.0,
            "mean_abs_diff_avg": mean_abs,
            "worst_frame": worst,
            "videos": videos,
            "per_frame_metrics_json": str(game_dir / "per_frame_metrics.json"),
        }

        (game_dir / "per_frame_metrics.json").write_text(
            json.dumps({"frames": per_frame}, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (game_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

        # Keep extracted rom path for traceability.
        if extracted_dir is not None:
            (game_dir / "extracted_rom.txt").write_text(str(rom_run_path) + "\n", encoding="utf-8")

        return result


def write_markdown_report(path: Path, results: List[Dict[str, object]], failures: List[Dict[str, str]]) -> None:
    lines: List[str] = []
    lines.append("# Intro Video Comparison Report")
    lines.append("")
    lines.append(f"- Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"- Games attempted: {len(results) + len(failures)}")
    lines.append(f"- Games succeeded: {len(results)}")
    lines.append(f"- Games failed: {len(failures)}")
    lines.append("")
    lines.append("## Results")
    lines.append("")
    lines.append("| Game | Changed Frame Ratio | Pixel Diff Ratio | Mean Abs Diff | Worst Frame | Side-by-Side | Triptych |")
    lines.append("|---|---:|---:|---:|---:|---|---|")
    for r in results:
        vids = r.get("videos", {})
        sbs = vids.get("side_by_side_video", "")
        tri = vids.get("triptych_video", "")
        lines.append(
            "| {game} | {cfr:.4f} | {pdr:.4f} | {mad:.4f} | {wf} | {sbs} | {tri} |".format(
                game=r["game_id"],
                cfr=float(r.get("changed_frame_ratio", 0.0)),
                pdr=float(r.get("pixel_diff_ratio_all_frames", 0.0)),
                mad=float(r.get("mean_abs_diff_avg", 0.0)),
                wf=int((r.get("worst_frame") or {}).get("frame", -1)),
                sbs=sbs,
                tri=tri,
            )
        )
    lines.append("")

    if failures:
        lines.append("## Failures")
        lines.append("")
        for f in failures:
            lines.append(f"- `{f['game_id']}`: {f['error']}")
        lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture and compare intro videos across reference and candidate plugins.")
    parser.add_argument(
        "--manifest",
        default="tests/intro_video/games.tsv",
        help="TSV manifest with game_id, rom_pattern, rationale",
    )
    parser.add_argument("--rom-root", default="/home/auro/code/n64_roms", help="ROM root directory")
    parser.add_argument("--runtime-root", default="/home/auro/code/mupen", help="LLM runtime root")
    parser.add_argument(
        "--reference-plugin",
        default="/home/auro/code/realityvk-upstream/build-release/plugin/Release/mupen64plus-video-RealityVK.so",
        help="Reference plugin binary",
    )
    parser.add_argument(
        "--candidate-plugin",
        default="build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so",
        help="Candidate plugin binary",
    )
    parser.add_argument("--out-dir", default="build/intro-video-compare", help="Output directory")
    parser.add_argument(
        "--captures",
        type=int,
        default=90,
        help="Number of captured frames per backend/game (default ~=3 seconds at 60Hz with --step-frames=2)",
    )
    parser.add_argument("--step-frames", type=int, default=2, help="Emulated frames stepped between captures")
    parser.add_argument("--scale-div", type=int, default=2, help="Capture downscale divisor")
    parser.add_argument("--fps", type=int, default=30, help="Output video frame rate")
    parser.add_argument(
        "--warmup-frames",
        type=int,
        default=120,
        help="Frames stepped once after boot before sequence capture starts",
    )
    parser.add_argument("--max-games", type=int, default=0, help="Limit number of games (0 = all)")
    parser.add_argument("--games", default="", help="Comma-separated game ids to include")
    parser.add_argument("--launch-timeout-sec", type=float, default=90.0, help="Socket wait timeout")
    parser.add_argument("--command-timeout-sec", type=float, default=45.0, help="agentctl command timeout")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg", help="ffmpeg executable")
    parser.add_argument("--agent-retries", type=int, default=3, help="Retries for guarded agent commands")
    parser.add_argument(
        "--recovery-sleep-sec",
        type=float,
        default=0.2,
        help="Sleep between guarded command retries",
    )
    parser.add_argument("--fail-fast", action="store_true", help="Stop on first game failure")
    parser.add_argument("--verbose", action="store_true", help="Print per-command output where available")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    manifest = Path(args.manifest).resolve()
    rom_root = Path(args.rom_root).expanduser().resolve()
    runtime_root = Path(args.runtime_root).expanduser().resolve()
    reference_plugin_path = Path(args.reference_plugin).expanduser().resolve()
    candidate_plugin_path = Path(args.candidate_plugin).expanduser().resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.captures <= 0:
        raise SystemExit("--captures must be > 0")
    if args.step_frames <= 0:
        raise SystemExit("--step-frames must be > 0")
    if args.scale_div <= 0:
        raise SystemExit("--scale-div must be > 0")
    if args.fps <= 0:
        raise SystemExit("--fps must be > 0")
    if args.warmup_frames < 0:
        raise SystemExit("--warmup-frames must be >= 0")
    if args.agent_retries < 0:
        raise SystemExit("--agent-retries must be >= 0")
    if args.recovery_sleep_sec < 0:
        raise SystemExit("--recovery-sleep-sec must be >= 0")

    entries = load_manifest(manifest)
    if not entries:
        raise SystemExit(f"no entries loaded from manifest: {manifest}")

    if args.games:
        wanted = {g.strip() for g in args.games.split(",") if g.strip()}
        entries = [e for e in entries if e.game_id in wanted]
    if args.max_games > 0:
        entries = entries[: args.max_games]

    # Validate ffmpeg early.
    run_cmd([args.ffmpeg_bin, "-version"], check=True, quiet=not args.verbose)

    meta = {
        "manifest": str(manifest),
        "rom_root": str(rom_root),
        "runtime_root": str(runtime_root),
        "reference_plugin": str(reference_plugin_path),
        "candidate_plugin": str(candidate_plugin_path),
        "captures": args.captures,
        "step_frames": args.step_frames,
        "scale_div": args.scale_div,
        "fps": args.fps,
        "warmup_frames": args.warmup_frames,
        "games_selected": [e.game_id for e in entries],
        "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (out_dir / "run_meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    results: List[Dict[str, object]] = []
    failures: List[Dict[str, str]] = []

    for i, game in enumerate(entries, start=1):
        print(f"[{i}/{len(entries)}] {game.game_id}: resolving ROM")
        try:
            rom_path = resolve_rom(rom_root, game.rom_pattern)
            print(f"[{i}/{len(entries)}] {game.game_id}: capturing ({rom_path.name})")
            result = compare_game(
                game=game,
                rom_path=rom_path,
                out_dir=out_dir,
                reference_plugin_path=reference_plugin_path,
                candidate_plugin_path=candidate_plugin_path,
                runtime_root=runtime_root,
                captures=args.captures,
                step_frames=args.step_frames,
                scale_div=args.scale_div,
                fps=args.fps,
                ffmpeg_bin=args.ffmpeg_bin,
                launch_timeout_sec=args.launch_timeout_sec,
                command_timeout_sec=args.command_timeout_sec,
                launch_quiet=not args.verbose,
                command_quiet=not args.verbose,
                agent_retries=args.agent_retries,
                recovery_sleep_sec=args.recovery_sleep_sec,
                warmup_frames=args.warmup_frames,
            )
            results.append(result)
            print(
                "[{}/{}] {}: done (changed_frame_ratio={:.4f}, pixel_diff_ratio={:.4f})".format(
                    i,
                    len(entries),
                    game.game_id,
                    float(result["changed_frame_ratio"]),
                    float(result["pixel_diff_ratio_all_frames"]),
                )
            )
        except Exception as exc:  # pylint: disable=broad-except
            msg = str(exc)
            print(f"[{i}/{len(entries)}] {game.game_id}: FAILED: {msg}", file=sys.stderr)
            failures.append({"game_id": game.game_id, "error": msg})
            if args.fail_fast:
                break

    overall = {
        "run_meta": meta,
        "succeeded": len(results),
        "failed": len(failures),
        "results": results,
        "failures": failures,
        "finished_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (out_dir / "results.json").write_text(json.dumps(overall, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_markdown_report(out_dir / "REPORT.md", results, failures)

    print(f"Report: {out_dir / 'REPORT.md'}")
    print(f"JSON:   {out_dir / 'results.json'}")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
