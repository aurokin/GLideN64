import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Optional


class PaperMarioParityDryRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.script = cls.repo_root / "scripts" / "paper_mario_parity.sh"

    def _write_manifest(self, path: Path, rom_path: Path) -> None:
        # scenario_id\trom\tframes\targs
        path.write_text(
            f"paper_mario_intro\t{rom_path}\t5\t\n",
            encoding="utf-8",
        )

    def _run_dry(
        self,
        profile: str,
        *,
        deep_override: Optional[str] = None,
        reference_plugin_name: str = "reference.so",
        frames_override: Optional[int] = None,
    ) -> dict:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            run_root = root / "run"
            cache_root = root / "cache"
            manifest = root / "scenarios.tsv"
            rom = root / "paper-mario.z64"
            candidate_plugin = root / "candidate.so"
            reference_plugin = root / reference_plugin_name
            candidate_core = root / "candidate-core.so"
            reference_core = root / "reference-core.so"
            dry_out = run_root / "dry-run.json"

            rom.write_bytes(b"rom")
            candidate_plugin.write_bytes(b"candidate")
            reference_plugin.write_bytes(b"reference")
            candidate_core.write_bytes(b"candidate-core")
            reference_core.write_bytes(b"reference-core")
            self._write_manifest(manifest, rom)

            env = os.environ.copy()
            for key in (
                "REALITYVK_PM_DEEP_TELEMETRY",
                "REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE",
                "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL",
                "REALITYVK_PM_DEEP_TELEMETRY_DIFF_PLAYBOOK",
            ):
                env.pop(key, None)
            env.update(
                {
                    "REALITYVK_PM_PROFILE": profile,
                    "REALITYVK_PM_DRY_RUN": "1",
                    "REALITYVK_PM_DRY_RUN_OUT": str(dry_out),
                    "REALITYVK_PM_AUTO_COMPARE_VIEW": "0",
                    "REALITYVK_PM_KNOB_TRACK_ENABLE": "0",
                    "REALITYVK_PM_MANIFEST": str(manifest),
                    "REALITYVK_PM_SCENARIO_ID": "paper_mario_intro",
                    "REALITYVK_PM_RUN_ROOT": str(run_root),
                    "REALITYVK_PM_CACHE_ROOT": str(cache_root),
                    "REALITYVK_PM_CANDIDATE_PLUGIN": str(candidate_plugin),
                    "REALITYVK_PM_REFERENCE_PLUGIN": str(reference_plugin),
                    "REALITYVK_PM_CANDIDATE_CORELIB": str(candidate_core),
                    "REALITYVK_PM_REFERENCE_CORELIB": str(reference_core),
                }
            )
            if deep_override is not None:
                env["REALITYVK_PM_DEEP_TELEMETRY"] = deep_override
            if frames_override is not None:
                env["REALITYVK_PM_FRAMES_OVERRIDE"] = str(frames_override)

            subprocess.run([str(self.script)], env=env, check=True, cwd=self.repo_root)
            self.assertTrue(dry_out.is_file())
            return json.loads(dry_out.read_text(encoding="utf-8"))

    def _capture_env(self, payload: dict, label: str) -> list[str]:
        captures = payload.get("captures", [])
        for capture in captures:
            if isinstance(capture, dict) and capture.get("label") == label:
                env = capture.get("env", [])
                if isinstance(env, list):
                    return [str(item) for item in env]
        return []

    def _capture_env_value(self, payload: dict, label: str, key: str) -> Optional[str]:
        prefix = f"{key}="
        for item in self._capture_env(payload, label):
            if item.startswith(prefix):
                return item[len(prefix) :]
        return None

    def test_basic_profile_defaults_to_no_deep_telemetry(self):
        payload = self._run_dry("basic")
        self.assertEqual(payload.get("profile"), "basic")
        self.assertEqual(int(payload.get("deep_telemetry", -1)), 0)

        candidate_env = self._capture_env(payload, "candidate")
        self.assertFalse(any(item.startswith("REALITYVK_RVK2_TRACE_FILE=") for item in candidate_env))

    def test_deep_profile_defaults_to_deep_telemetry(self):
        payload = self._run_dry("deep")
        self.assertEqual(payload.get("profile"), "deep")
        self.assertEqual(int(payload.get("deep_telemetry", -1)), 1)

        candidate_env = self._capture_env(payload, "candidate")
        self.assertTrue(any(item.startswith("REALITYVK_RVK2_TRACE_FILE=") for item in candidate_env))

    def test_explicit_deep_telemetry_override_wins(self):
        payload = self._run_dry("deep", deep_override="0")
        self.assertEqual(payload.get("profile"), "deep")
        self.assertEqual(int(payload.get("deep_telemetry", -1)), 0)

        candidate_env = self._capture_env(payload, "candidate")
        self.assertFalse(any(item.startswith("REALITYVK_RVK2_TRACE_FILE=") for item in candidate_env))

    def test_reference_gliden64_disables_reference_non_black_check_only(self):
        payload = self._run_dry("basic", reference_plugin_name="mupen64plus-video-GLideN64.so")

        reference_require_non_black = self._capture_env_value(
            payload, "reference", "REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE"
        )
        candidate_require_non_black = self._capture_env_value(
            payload, "candidate", "REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE"
        )
        self.assertEqual(reference_require_non_black, "0")
        self.assertEqual(candidate_require_non_black, "1")

    def test_frames_override_updates_effective_frames(self):
        payload = self._run_dry("basic", frames_override=17)
        self.assertEqual(int(payload.get("frames", -1)), 17)


if __name__ == "__main__":
    unittest.main()
