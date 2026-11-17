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
        """Each outcome is (exit code, aggregate status[, seconds the controller runs])."""
        clock = [0.0]
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            self.assertEqual(command[0], "timeout")
            self.assertEqual(command[2], "--kill-after=2s")
            limit = float(command[3][:-1])
            self.assertLessEqual(limit + 2.0, 240.0 - clock[0])
            self.assertEqual(command[-4:], ["/deploy/single_user_deploy.sh", "-s", "-w", "/work"])
            rc, status, *runs = outcomes[min(len(calls) - 1, len(outcomes) - 1)]
            runs = runs[0] if runs else 30.0
            if runs > limit:
                # timeout(1) stops the controller: exit 124, no aggregate record.
                clock[0] += limit
                return subprocess.CompletedProcess(command, 124, "status output\n", "")
            clock[0] += runs
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
        self.assertEqual(len(calls), 8)
        self.assertLessEqual(elapsed, 240.0)

    def test_a_stop_that_waits_for_the_grapher_completes_in_one_run(self):
        # The controller waits for the keepers to exit; a keeper waiting for
        # the grapher to confirm its last chunk takes about 90 s here, three
        # of its own 30 s stop attempts.
        result, calls, elapsed = self.exercise([(0, 0, 90.0)])
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(elapsed, 90.0)

    def test_a_stop_still_running_at_the_guard_fails(self):
        result, calls, elapsed = self.exercise([(0, 0, 300.0)])
        self.assertEqual(result, 1)
        self.assertEqual(len(calls), 1)
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
