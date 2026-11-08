import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

module_spec = importlib.util.spec_from_file_location(
    "managed_stop_check", Path(__file__).with_name("managed_stop_check.py"))
stop = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(stop)


class ManagedStopCheck(unittest.TestCase):
    def exercise(self, outcomes):
        clock = [0.0]
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            self.assertEqual(command[0], "timeout")
            self.assertLessEqual(float(command[3][:-1]), 240.0 - clock[0])
            self.assertEqual(command[-4:], ["/deploy/single_user_deploy.sh", "-s", "-w", "/work"])
            rc, status = outcomes[min(len(calls) - 1, len(outcomes) - 1)]
            clock[0] += 30.0
            record = {"operation": "stop", "aggregate": True, "status": status}
            return subprocess.CompletedProcess(command, rc, "status output\n" + json.dumps(record) + "\n", "")

        with tempfile.TemporaryDirectory() as directory:
            result = stop.managed_stop("/work", "/deploy/single_user_deploy.sh",
                                       Path(directory) / "stop.log", runner=run,
                                       now=lambda: clock[0],
                                       pause=lambda seconds: clock.__setitem__(0, clock[0] + seconds))
        return result, calls, clock[0]

    def test_retry_incomplete_uses_the_same_work_and_controller(self):
        result, calls, _ = self.exercise([(27, -27), (0, 0)])
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 2)

    def test_original_240_second_guard_is_not_reset_between_attempts(self):
        result, calls, elapsed = self.exercise([(27, -27)])
        self.assertEqual(result, 1)
        self.assertEqual(len(calls), 7)
        self.assertLessEqual(elapsed, 240.0)

    def test_a_command_timeout_is_not_a_completed_incomplete_attempt(self):
        result, calls, _ = self.exercise([(124, -27)])
        self.assertEqual(result, 1)
        self.assertEqual(len(calls), 1)

    def test_conflicting_command_and_aggregate_status_is_not_success(self):
        result, calls, _ = self.exercise([(0, -27)])
        self.assertEqual(result, 1)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
