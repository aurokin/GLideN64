import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class PaperMarioShadowOracleDryRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.script = cls.repo_root / "scripts" / "paper_mario_shadow_oracle.sh"

    def test_dry_run_emits_plan_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            dry_out = root / "shadow-oracle-dry-run.json"

            env = os.environ.copy()
            env.update(
                {
                    "REALITYVK_PM_SCENARIO_ID": "paper_mario_intro",
                    "REALITYVK_PM_PROFILE": "deep",
                }
            )

            subprocess.run(
                [
                    str(self.script),
                    "--frames",
                    "20",
                    "--retry-count",
                    "0",
                    "--threshold",
                    "21",
                    "--min-area",
                    "96",
                    "--history-frame-window",
                    "4",
                    "--ignore-box",
                    "1,2,3,4",
                    "--ignore-box",
                    "5,6,7,8",
                    "--dry-run",
                    "--dry-run-out",
                    str(dry_out),
                ],
                cwd=self.repo_root,
                env=env,
                check=True,
            )

            self.assertTrue(dry_out.is_file())
            payload = json.loads(dry_out.read_text(encoding="utf-8"))
            self.assertEqual(payload.get("schema"), "paper_mario_shadow_oracle_dry_run_v1")
            self.assertEqual(payload.get("scenario_id"), "paper_mario_intro")
            self.assertEqual(payload.get("profile"), "deep")
            self.assertEqual(payload.get("frames"), 20)
            self.assertEqual(payload.get("retry_count"), 0)
            self.assertEqual(payload.get("threshold"), 21)
            self.assertEqual(payload.get("min_area"), 96)
            self.assertEqual(payload.get("history_frame_window"), 4)
            self.assertIn("1,2,3,4", payload.get("ignore_boxes", []))
            self.assertIn("5,6,7,8", payload.get("ignore_boxes", []))


if __name__ == "__main__":
    unittest.main()
