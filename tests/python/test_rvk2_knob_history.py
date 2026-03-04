import json
import subprocess
import tempfile
import unittest
from pathlib import Path


class KnobHistoryToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.tool = cls.repo_root / "scripts" / "rvk2_knob_history.py"

    def _run(self, *args):
        cmd = ["python3", str(self.tool), *args]
        return subprocess.run(cmd, check=True, capture_output=True, text=True)

    def test_fingerprint_is_stable_independent_of_kv_order(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            out_a = Path(tmpdir) / "a.json"
            out_b = Path(tmpdir) / "b.json"

            fp_a = self._run(
                "fingerprint",
                "--output-json",
                str(out_a),
                "--scenario-id",
                "paper_mario_intro",
                "--profile",
                "deep",
                "--kv",
                "foo=1",
                "--kv",
                "bar=2",
            ).stdout.strip()
            fp_b = self._run(
                "fingerprint",
                "--output-json",
                str(out_b),
                "--scenario-id",
                "paper_mario_intro",
                "--profile",
                "deep",
                "--kv",
                "bar=2",
                "--kv",
                "foo=1",
            ).stdout.strip()

            self.assertEqual(fp_a, fp_b)
            self.assertTrue(out_a.is_file())
            payload = json.loads(out_a.read_text(encoding="utf-8"))
            self.assertEqual(payload["fingerprint"], fp_a)

    def test_record_and_recent(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            history = Path(tmpdir) / "history.tsv"
            snapshot = Path(tmpdir) / "knobs.json"
            snapshot.write_text("{}\n", encoding="utf-8")
            metrics = Path(tmpdir) / "metrics.json"
            metrics.write_text(json.dumps({"rmse": 0.1, "mae": 0.2, "pass": True}), encoding="utf-8")

            self._run(
                "record",
                "--history",
                str(history),
                "--run-stamp",
                "20260304-000000Z",
                "--scenario-id",
                "paper_mario_intro",
                "--profile",
                "basic",
                "--fingerprint",
                "abc",
                "--deep-telemetry",
                "0",
                "--visual-exit",
                "1",
                "--git-sha",
                "deadbeef",
                "--snapshot",
                str(snapshot),
                "--metrics",
                str(metrics),
            )

            self._run(
                "record",
                "--history",
                str(history),
                "--run-stamp",
                "20260304-000100Z",
                "--scenario-id",
                "paper_mario_intro",
                "--profile",
                "basic",
                "--fingerprint",
                "abc",
                "--deep-telemetry",
                "0",
                "--visual-exit",
                "0",
                "--git-sha",
                "feedface",
                "--snapshot",
                str(snapshot),
            )

            recent_raw = self._run(
                "recent",
                "--history",
                str(history),
                "--scenario-id",
                "paper_mario_intro",
                "--fingerprint",
                "abc",
                "--window",
                "5",
            ).stdout
            recent = json.loads(recent_raw)
            self.assertEqual(recent["same_fingerprint_count"], 2)
            self.assertEqual(recent["last_same_run_stamp_utc"], "20260304-000100Z")

            summary = self._run(
                "summary",
                "--history",
                str(history),
                "--scenario-id",
                "paper_mario_intro",
                "--limit",
                "5",
            ).stdout
            self.assertIn("fingerprint", summary)
            self.assertIn("20260304-000100Z", summary)


if __name__ == "__main__":
    unittest.main()
