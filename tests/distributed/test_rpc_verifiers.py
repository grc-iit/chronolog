import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import rpc_config_audit as config_audit
import rpc_shutdown_data_check as data_check


class ConfigAudit(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        (self.work / "monitor/lifecycle").mkdir(parents=True)
        self.manifest = dict(id="deployment", records=[])
        self.expected = dict(visor=1, keeper=1, grapher=1, player=1)
        for index, component in enumerate(self.expected, 1):
            config = self.work / (component + ".json")
            active = {"rpc": {"protocol_conf": "ofi+sockets"}}
            if component in ("keeper", "grapher"):
                active["ExtractionModule"] = {"extraction_protocol": "ofi+sockets"}
            text = json.dumps({"chrono_" + component: active,
                               "unused": {"protocol_conf": "different", "extraction_protocol": "different"}})
            config.write_text(text)
            executable = "/bin/chrono-" + component
            record = dict(component=component, host="host", pid=index, uid=1, executable=executable,
                          argv=[executable, "--config", str(config)], config=str(config),
                          start_ticks="123", boot_id="boot", config_bytes=text,
                          status_file=str(config) + ".lifecycle.json")
            status = {key: record[key] for key in ("pid", "uid", "executable", "config", "host", "start_ticks", "boot_id")}
            status.update(deployment_id="deployment", native_proof={"generation": 1})
            Path(record["status_file"]).write_text(json.dumps(status))
            self.manifest["records"].append(record)
        self.save()

    def save(self):
        (self.work / "monitor/lifecycle/deployment.json").write_text(json.dumps(self.manifest))

    def audit(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return config_audit.audit(self.work, "ofi+sockets", self.expected)

    def test_actual_inventory_audits_every_active_component(self):
        self.assertEqual(self.audit(), 4)

    def test_empty_inventory_is_not_a_successful_audit(self):
        self.manifest["records"] = []
        self.save()
        with self.assertRaises(ValueError):
            self.audit()

    def test_changed_config_bytes_are_rejected(self):
        Path(self.manifest["records"][0]["config"]).write_text("{}")
        with self.assertRaises(ValueError):
            self.audit()

    def test_native_pid_binding_is_required(self):
        record = self.manifest["records"][0]
        path = Path(record["status_file"])
        status = json.loads(path.read_text())
        status["pid"] += 100
        path.write_text(json.dumps(status))
        with self.assertRaises(ValueError):
            self.audit()

    def test_wrong_keeper_extraction_protocol_is_rejected(self):
        record = self.manifest["records"][1]
        data = json.loads(record["config_bytes"])
        data["chrono_keeper"]["ExtractionModule"]["extraction_protocol"] = "ofi+tcp"
        record["config_bytes"] = json.dumps(data)
        Path(record["config"]).write_text(record["config_bytes"])
        self.save()
        with self.assertRaises(ValueError):
            self.audit()


class ShutdownTiming(unittest.TestCase):
    def test_release_acceptance_can_precede_normal_chunk_decay(self):
        self.assertEqual(data_check.unsealed_until([(101_000_000_000, 2, 1)], 102_000_000_000, 10, 15),
                         117_000_000_000)

    def test_late_or_missing_shutdown_timestamp_cannot_prove_unsealed_data(self):
        holder = "Chronicle=test, Story=retained, StoryID=1\n"
        late = holder + "[1970-01-01 00:01:57.000] [KeeperDataStore] Initiating shutdown of DataCollection."
        with self.assertRaises(ValueError):
            data_check.shutdown_before_seal(late, "test", 117_000_000_000)
        with self.assertRaises(ValueError):
            data_check.shutdown_before_seal(holder, "test", 117_000_000_000)

    def test_selected_observation_keeps_exact_shutdown_and_holder_lines(self):
        holder = "Chronicle=test, Story=retained, StoryID=1"
        shutdown = "[1970-01-01 00:01:56.998] [KeeperDataStore] Initiating shutdown of DataCollection."
        observation = data_check.shutdown_before_seal(holder + "\n" + shutdown, "test", 117_000_000_000)
        self.assertEqual(observation["shutdown_timestamp_ns"], 116_998_000_000)
        self.assertEqual(observation["shutdown_timestamp_upper_ns"], 116_999_000_000)
        self.assertEqual(observation["shutdown_line"], shutdown)
        self.assertEqual(observation["holder_line"], holder)

    def test_summary_preserves_causal_evidence_after_source_logs_are_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "keeper.log"
            holder = "Chronicle=test, Story=retained, StoryID=1"
            shutdown = "[1970-01-01 00:01:56.998] [KeeperDataStore] Initiating shutdown of DataCollection."
            text = holder + "\n" + shutdown + "\n"
            log.write_text(text)
            record = dict(component="keeper", host="c2", pid=123, uid=1,
                          executable="/bin/chrono-keeper", config="/conf/keeper.json",
                          start_ticks="456", boot_id="boot")
            observation = data_check.keeper_shutdown_observation(
                record, [(log, text)], "test", 117_000_000_000,
                116_000_000_000, 118_000_000_000, 7, "deployment")
            summary = dict(chronicle="test", unsealed_until=117_000_000_000,
                           holder_observations=[observation])
            saved = data_check.persist_summary(summary, root)
            log.write_text("later NFS deployment replaced the log\n")
            value = json.loads(saved.read_text())
            proof = value["holder_observations"][0]
            self.assertEqual(proof["identity"], dict(record, generation=7, deployment_id="deployment"))
            self.assertEqual(proof["shutdown_timestamp_upper_ns"], 116_999_000_000)
            self.assertEqual(proof["shutdown_source"], dict(file=str(log), line_number=2, line=shutdown))
            self.assertEqual(proof["holder_source"], dict(file=str(log), line_number=1, line=holder))
            self.assertEqual(json.loads((root / "shutdown_data_check.json").read_text()), value)
            self.assertEqual(saved.stat().st_mode & 0o222, 0)
            with self.assertRaises(FileExistsError):
                data_check.persist_summary(summary, root)
            self.assertEqual(json.loads(saved.read_text()), value)

    def test_only_actual_holder_and_conservative_timestamp_can_prove_unsealed_data(self):
        text = "Chronicle=test, Story=retained, StoryID=1\n"
        text += "[1970-01-01 00:01:56.998] [KeeperDataStore] Initiating shutdown of DataCollection."
        self.assertTrue(data_check.shutdown_before_seal(text, "test", 117_000_000_000))
        self.assertFalse(data_check.shutdown_before_seal(text, "other", 117_000_000_000))
        with self.assertRaises(ValueError):
            data_check.shutdown_before_seal(text, "test", 117_000_000_000, 118_000_000_000, 119_000_000_000)


if __name__ == "__main__":
    unittest.main()
