import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("benchmark_ndk", Path(__file__).parents[1] / "tools/benchmark_ndk.py")
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class MetricTests(unittest.TestCase):
    def test_direction_and_sender_retransmissions(self):
        payload = {"end": {
            "sum_sent": {"bits_per_second": 9e9, "retransmits": 12},
            "sum_received": {"bits_per_second": 8e9},
            "cpu_utilization_percent": {"host_total": 15, "remote_total": 20},
        }}
        tx, rx = benchmark.metrics(payload, "tx"), benchmark.metrics(payload, "rx")
        self.assertEqual((tx["local_role"], tx["local_bps"]), ("sender", 9e9))
        self.assertEqual((rx["local_role"], rx["local_bps"]), ("receiver", 8e9))
        self.assertEqual(rx["retransmits"], 12)  # Remote sender in reverse mode.
        self.assertEqual(rx["host_cpu_percent"], 15)  # Host always means local.

    def test_missing_optional_metrics_are_not_zero(self):
        result = benchmark.metrics({"end": {"sum_received": {"bits_per_second": 0}}}, "rx")
        self.assertEqual(result["local_bps"], 0)
        self.assertIsNone(result["retransmits"])
        self.assertIsNone(result["host_cpu_percent"])
        with self.assertRaises(ValueError):
            benchmark.metrics({"end": {"sum_received": {"bits_per_second": 8e9}}}, "tx")

    def test_invalid_results_are_errors(self):
        invalid = [[], {}, {"error": "connection refused"}, {"end": {"sum_sent": None}}]
        invalid += [{"end": {"sum_sent": {"bits_per_second": value}}}
                    for value in (None, "9e9", True, -1, float("nan"), float("inf"))]
        for payload in invalid:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                benchmark.metrics(payload, "tx")

    def test_failed_runs_preserve_artifacts_and_continue(self):
        def fake_run(command, timeout):
            result = {"command": command, "returncode": 0, "stdout": "", "stderr": "", "error": None}
            if "-c" in command:
                result["stdout"] = ('{"end":{"sum_received":{"bits_per_second":8e9}},"unvalidated_field":'
                                    + invalid_float + '}') if "-R" in command else '{"error":"server refused"}'
            return result

        for invalid_float in ("NaN", "1e999", "-1e999"):
            with self.subTest(value=invalid_float), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "results"
                arguments = ["benchmark", "--host", "192.0.2.1", "--interface", "en10",
                             "--reps", "1", "--streams", "1", "--output", str(output)]
                with patch("sys.argv", arguments), patch.object(benchmark, "run", fake_run), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(benchmark.main(), 1)
                summary = json.loads((output / "summary.json").read_text())
                self.assertEqual(len(summary), 2)
                for row in summary:
                    self.assertEqual(row["status"], "failed")
                    self.assertIsNone(row["local_bps"])
                    self.assertTrue(row["error"])
                    artifact = json.loads((output / row["artifact"]).read_text())
                    self.assertTrue(artifact["process"]["stdout"])
                    self.assertEqual(len(artifact["before"]), 2)
                    self.assertEqual(len(artifact["after"]), 2)


if __name__ == "__main__":
    unittest.main()
