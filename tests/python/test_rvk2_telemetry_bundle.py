import importlib.util
import tempfile
import unittest
from pathlib import Path


def _load_module():
    repo_root = Path(__file__).resolve().parents[2]
    module_path = repo_root / "scripts" / "rvk2_telemetry_bundle.py"
    spec = importlib.util.spec_from_file_location("rvk2_telemetry_bundle", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("failed to load rvk2_telemetry_bundle module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TelemetryBundleFrameMatchingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bundle = _load_module()

    def _build_signals(self, *, overwrite_summary, triangle_summary, missing_focus):
        return self.bundle._build_signals(
            last_record={},
            replay_summary={},
            launch_summary={"readback_marker_count": 1},
            history_merge_summary={},
            overwrite_summary=overwrite_summary,
            triangle_packet_summary=triangle_summary,
            depth_summary=None,
            metrics=None,
            command_census=None,
            diff_playbook_summary=None,
            missing_region_focus=missing_focus,
            executor_present_compare=None,
        )

    def test_parse_overwrite_log_emits_per_frame_packet_profiles(self):
        payload = (
            "frame=10\tsource_packet_id=42\top_name=texrect\tphase=1\t"
            "texel=0x00000000\tcombiner=0x00000000\tblender=0x00000000\tfinal=0x00000000\t"
            "overwrite_to_black=1\tquantized_to_black=0\n"
            "frame=11\tsource_packet_id=42\top_name=texrect\tphase=1\t"
            "texel=0x00000000\tcombiner=0x00000000\tblender=0x00000000\tfinal=0x00000000\t"
            "overwrite_to_black=1\tquantized_to_black=0\n"
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "overwrite.tsv"
            log_path.write_text(payload, encoding="utf-8")
            summary = self.bundle._parse_overwrite_log(log_path)

        self.assertEqual(summary["source_packet_stage_profiles_by_frame_count"], 2)
        rows = summary["source_packet_stage_profiles_by_frame"]
        keyed = {(int(row["frame_id"]), int(row["source_packet_id"])): row for row in rows}
        self.assertIn((10, 42), keyed)
        self.assertIn((11, 42), keyed)

    def test_parse_overwrite_log_focus_cluster_state_profiles(self):
        payload = (
            "frame=20\tsource_packet_id=88\top_name=fill\tphase=4\t"
            "texel=0x00000000\tcombiner=0x00000000\tblender=0x00000000\tfinal=0x00000000\t"
            "overwrite_to_black=1\tquantized_to_black=0\tfocus_cluster=1\tprev_write_mask=1\t"
            "combine_mux=0x00FFFFFFFFFCF87C\tother_modes=0x00308C7F00000000\tblend_params=0x11223344\t"
            "cycle_type=3\talpha_compare=0\tcvg_dest=0\tblend_mask=0xA5\tforce_blender=1\t"
            "depth_test=0\tdepth_compare_enable=0\tdepth_update_enable=0\tfill_color=0x01020304\t"
            "tile_format=0\ttile_size=3\ttile_line=0\ttile_tmem=0\n"
            "frame=21\tsource_packet_id=88\top_name=fill\tphase=4\t"
            "texel=0x00000000\tcombiner=0x00000000\tblender=0x00000000\tfinal=0x00000000\t"
            "overwrite_to_black=1\tquantized_to_black=0\tfocus_cluster=1\tprev_write_mask=1\t"
            "combine_mux=0x00FFFFFFFFFCF87C\tother_modes=0x00308C7F00000000\tblend_params=0x11223344\t"
            "cycle_type=3\talpha_compare=0\tcvg_dest=0\tblend_mask=0xA5\tforce_blender=1\t"
            "depth_test=0\tdepth_compare_enable=0\tdepth_update_enable=0\tfill_color=0x01020304\t"
            "tile_format=0\ttile_size=3\ttile_line=0\ttile_tmem=0\n"
            "frame=21\tsource_packet_id=99\top_name=texrect\tphase=1\t"
            "texel=0x00000000\tcombiner=0x00000000\tblender=0x00000000\tfinal=0x00000000\t"
            "overwrite_to_black=1\tquantized_to_black=0\tfocus_cluster=2\tprev_write_mask=0\t"
            "combine_mux=0x00FFFFFFFFFCF87C\tother_modes=0x00208C7F00000000\tblend_params=0x55667788\t"
            "cycle_type=2\talpha_compare=0\tcvg_dest=0\tblend_mask=0x5A\tforce_blender=1\t"
            "tile_format=2\ttile_size=1\ttile_line=32\ttile_tmem=96\ttile_palette=7\t"
            "texture_image_format=2\ttexture_image_size=1\ttexture_image_width=320\t"
            "texture_image_address=0x00112233\n"
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "overwrite.tsv"
            log_path.write_text(payload, encoding="utf-8")
            summary = self.bundle._parse_overwrite_log(log_path)

        fill = summary["focus_clusters"]["fill"]
        texrect = summary["focus_clusters"]["texrect"]

        self.assertEqual(fill["writes"], 2)
        self.assertEqual(fill["top_states"][0]["count"], 2)
        self.assertEqual(fill["top_states"][0]["blend_mask"], "0xA5")
        self.assertEqual(fill["top_states"][0]["fill_color"], "0x01020304")
        self.assertEqual(fill["top_source_packets"][0]["source_packet_id"], 88)
        self.assertEqual(fill["top_source_packets"][0]["count"], 2)

        self.assertEqual(texrect["writes"], 1)
        self.assertEqual(texrect["top_states"][0]["tile_palette"], 7)
        self.assertEqual(texrect["top_states"][0]["texture_image_width"], 320)
        self.assertEqual(texrect["top_source_packets"][0]["source_packet_id"], 99)

    def test_missing_region_prefers_frame_packet_match(self):
        overwrite_summary = {
            "record_count": 1,
            "black_write_record_count": 1,
            "overwrite_record_count": 1,
            "source_packet_stage_profiles": [
                {
                    "source_packet_id": 42,
                    "record_count": 10,
                    "overwrite_count": 3,
                }
            ],
            "source_packet_stage_profiles_by_frame": [
                {
                    "source_packet_id": 42,
                    "frame_id": 10,
                    "record_count": 5,
                    "overwrite_count": 2,
                }
            ],
        }
        triangle_summary = {
            "record_count": 1,
            "source_packet_profiles": [
                {
                    "source_packet_id": 42,
                    "record_count": 30,
                    "sample_candidates": 100,
                    "writes": 20,
                    "zero_sample_reason_counts": {},
                    "dominant_zero_sample_reason": "other",
                }
            ],
            "source_packet_profiles_by_frame": [
                {
                    "source_packet_id": 42,
                    "frame_id": 10,
                    "record_count": 9,
                    "sample_candidates": 25,
                    "writes": 7,
                    "zero_sample_reason_counts": {},
                    "dominant_zero_sample_reason": "other",
                }
            ],
        }
        missing_focus = {
            "frame_id": 10,
            "counts": {},
            "missing_write_attribution": {
                "missing_with_write_packet_hits_ranked": [
                    {
                        "source_packet_id": 42,
                        "frame_id": 10,
                        "work_hits": 4,
                        "pixel_hits": 8,
                    }
                ]
            },
        }

        signals = self._build_signals(
            overwrite_summary=overwrite_summary,
            triangle_summary=triangle_summary,
            missing_focus=missing_focus,
        )
        summary = signals["missing_region"]["missing_with_write_packet_stage_summary"]

        self.assertEqual(summary["rows_considered"], 1)
        self.assertEqual(summary["rows_matched_to_overwrite_log_frame_packet"], 1)
        self.assertEqual(summary["rows_matched_to_overwrite_log_packet_only"], 0)
        self.assertEqual(summary["rows_matched_to_triangle_packet_log_frame_packet"], 1)
        self.assertEqual(summary["rows_matched_to_triangle_packet_log_packet_only"], 0)

    def test_missing_region_falls_back_to_packet_only_when_frame_missing(self):
        overwrite_summary = {
            "record_count": 1,
            "black_write_record_count": 1,
            "overwrite_record_count": 1,
            "source_packet_stage_profiles": [
                {
                    "source_packet_id": 77,
                    "record_count": 6,
                    "overwrite_count": 1,
                }
            ],
            "source_packet_stage_profiles_by_frame": [],
        }
        triangle_summary = {
            "record_count": 1,
            "source_packet_profiles": [
                {
                    "source_packet_id": 77,
                    "record_count": 12,
                    "sample_candidates": 20,
                    "writes": 3,
                    "zero_sample_reason_counts": {},
                    "dominant_zero_sample_reason": "other",
                }
            ],
            "source_packet_profiles_by_frame": [],
        }
        missing_focus = {
            "frame_id": 123,
            "counts": {},
            "missing_write_attribution": {
                "missing_with_write_packet_hits_ranked": [
                    {
                        "source_packet_id": 77,
                        "frame_id": 123,
                        "work_hits": 1,
                        "pixel_hits": 1,
                    }
                ]
            },
        }

        signals = self._build_signals(
            overwrite_summary=overwrite_summary,
            triangle_summary=triangle_summary,
            missing_focus=missing_focus,
        )
        summary = signals["missing_region"]["missing_with_write_packet_stage_summary"]

        self.assertEqual(summary["rows_considered"], 1)
        self.assertEqual(summary["rows_matched_to_overwrite_log_frame_packet"], 0)
        self.assertEqual(summary["rows_matched_to_overwrite_log_packet_only"], 1)
        self.assertEqual(summary["rows_matched_to_triangle_packet_log_frame_packet"], 0)
        self.assertEqual(summary["rows_matched_to_triangle_packet_log_packet_only"], 1)


if __name__ == "__main__":
    unittest.main()
