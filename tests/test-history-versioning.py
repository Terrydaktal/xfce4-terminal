"""Independent contracts for history generations and per-revision build numbering."""

from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "scripts/fork_version.py"
if not ENGINE.exists():
    ENGINE = ROOT / "build_tools/fork_version.py"
SPEC = importlib.util.spec_from_file_location("fork_version", ENGINE)
versioning = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(versioning)


class HistoryVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="history-version-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        (self.root / "FORK-VERSION").write_text("1.2.0-fork0.2.0\n")
        (self.root / ".gitignore").write_text("build/\n__pycache__/\n")
        self.git("init", "-b", "main")
        self.git("add", ".")
        self.git("commit", "-qm", "Fixture upstream")
        (self.root / "FORK-BASE").write_text(self.git("rev-parse", "HEAD"))
        self.git("add", ".")
        self.git("commit", "-qm", "Fixture fork change")
        self.environment = {
            key: value
            for key, value in os.environ.items()
            if key not in ("BUILD_COUNTER_FILE", "CODEX_BUILD_VERSION_FILE")
        }

    def git(self, *arguments, input=None):
        return subprocess.check_output(
            [
                "git",
                "-C",
                str(self.root),
                "-c",
                "user.name=Terrydaktal",
                "-c",
                "user.email=9lewis9@gmail.com",
                "-c",
                "commit.gpgsign=false",
                "-c",
                "core.hooksPath=/dev/null",
                *arguments,
            ],
            input=input,
            text=True,
        ).strip()

    def command(self, operation, *arguments, check=True):
        return subprocess.run(
            [
                sys.executable,
                str(ENGINE),
                operation,
                "--root",
                str(self.root),
                *map(str, arguments),
            ],
            env=self.environment,
            capture_output=True,
            text=True,
            check=check,
            timeout=20,
        )

    def reserve(self, name="stamp"):
        return json.loads(
            self.command("reserve", "--output", self.root / "build" / name).stdout
        )

    def test_count_excludes_upstream_and_queries_are_read_only(self):
        index = self.root / ".git/index"
        before = index.stat().st_mtime_ns
        info = json.loads(self.command("source").stdout)
        self.assertEqual(info["source_version"], "1.2.0-fork0.2.1")
        self.assertFalse(info["dirty"])
        self.assertEqual(index.stat().st_mtime_ns, before)
        self.assertFalse((self.root / "build").exists())

    def test_large_counters_do_not_carry_between_components(self):
        for rebases, rewrites, count, expected in (
            (0, 2, 0, "0.2.0"),
            (4, 7, 99, "4.7.99"),
            (4, 7, 100, "4.7.100"),
            (4, 7, 100000, "4.7.100000"),
            (10000000000, 20000000000, 3, "10000000000.20000000000.3"),
        ):
            with self.subTest(rebases=rebases, rewrites=rewrites, count=count):
                self.assertEqual(
                    versioning.compose("3.7c", rebases, rewrites, count),
                    "3.7c-fork" + expected,
                )
        for counters in ((-1, 0, 0), (0, -1, 0), (0, 0, -1), (True, 0, 0)):
            with self.assertRaises(ValueError):
                versioning.compose("1.2.0", *counters)

    def test_commit_count_has_no_decimal_carry_or_fixed_ceiling(self):
        for count in (99, 100, 99999, 100000, 10**100):
            with self.subTest(count=count):
                self.assertEqual(
                    versioning.compose("1.2.0", 4, 7, count),
                    f"1.2.0-fork4.7.{count}",
                )

    def test_rebase_and_history_rewrite_counters_are_independent(self):
        (self.root / "FORK-VERSION").write_text("1.2.0-fork4.7.0\n")
        upstream = (self.root / "FORK-BASE").read_text().strip()
        tree = self.git("rev-parse", "HEAD^{tree}")
        new_base = self.git("commit-tree", tree, "-p", upstream, input="New upstream\n")
        new_tip = self.git("commit-tree", tree, "-p", new_base, input="Ported fork\n")
        self.git("update-ref", "refs/heads/main", new_tip)
        self.command("rebase", "--base", new_base, "--upstream", "1.3.0")
        self.assertEqual(self.git("rev-parse", "HEAD"), new_tip)
        self.assertEqual(self.reserve()["build_version"], "1.3.0-fork5.7.1-0")
        self.command("history")
        self.assertEqual(self.git("rev-parse", "HEAD"), new_tip)
        self.assertEqual(self.reserve()["build_version"], "1.3.0-fork5.8.1-1")
        events = json.loads((self.root / "FORK-HISTORY.json").read_text())["events"]
        self.assertEqual(
            [event["kind"] for event in events], ["rebase", "history-rewrite"]
        )
        self.assertEqual(events[0]["new_base"], new_base)

    def test_recording_the_same_upstream_upgrade_twice_is_rejected(self):
        anchor = (self.root / "FORK-BASE").read_text().strip()
        before = (self.root / "FORK-VERSION").read_bytes()
        result = self.command(
            "rebase", "--base", anchor, "--upstream", "1.2.0", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / "FORK-VERSION").read_bytes(), before)
        self.assertFalse((self.root / "FORK-HISTORY.json").exists())

    def test_source_commit_100_does_not_increment_either_history_counter(self):
        tip = self.git("rev-parse", "HEAD")
        tree = self.git("rev-parse", "HEAD^{tree}")
        for index in range(99):
            tip = self.git(
                "commit-tree", tree, "-p", tip, input=f"Fork commit {index}\n"
            )
        self.git("update-ref", "refs/heads/main", tip)
        self.assertEqual(
            json.loads(self.command("source").stdout)["source_version"],
            "1.2.0-fork0.2.100",
        )

    def test_rebase_rejects_non_first_parent_boundaries_before_writing(self):
        upstream = (self.root / "FORK-BASE").read_text().strip()
        tip = self.git("rev-parse", "HEAD")
        tree = self.git("rev-parse", "HEAD^{tree}")
        side = self.git("commit-tree", tree, "-p", upstream, input="Side upstream\n")
        merged = self.git(
            "commit-tree", tree, "-p", tip, "-p", side, input="Fixture merge\n"
        )
        self.git("update-ref", "refs/heads/main", merged)
        before = (self.root / "FORK-VERSION").read_bytes()
        result = self.command(
            "rebase", "--base", side, "--upstream", "1.3.0", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / "FORK-VERSION").read_bytes(), before)
        self.assertEqual((self.root / "FORK-BASE").read_text().strip(), upstream)
        self.assertFalse((self.root / "FORK-HISTORY.json").exists())

    def test_history_rewrite_cannot_change_the_upstream_boundary(self):
        upstream = (self.root / "FORK-BASE").read_text().strip()
        before = (self.root / "FORK-VERSION").read_bytes()
        result = self.command("history", "--base", upstream, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / "FORK-VERSION").read_bytes(), before)

    def test_corrupt_history_ledger_is_not_replaced(self):
        ledger = self.root / "FORK-HISTORY.json"
        ledger.write_text('{"events":42}')
        before = (self.root / "FORK-VERSION").read_bytes()
        result = self.command("history", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / "FORK-VERSION").read_bytes(), before)
        self.assertEqual(ledger.read_text(), '{"events":42}')

    def test_existing_scoped_build_numbers_are_preserved(self):
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        counter.write_text("4\n")
        state = counter.with_name(counter.name + ".versions.json")
        state.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "counters": {
                        "1.2.0-fork2.0.1": 9,
                        "1.2.0-fork0.2.1": 4,
                    },
                }
            )
        )
        old_info = json.loads(self.command("source").stdout)
        stamp = self.root / "build/stamp/BUILD-VERSION.json"
        stamp.parent.mkdir()
        stamp.write_text(
            json.dumps(
                {
                    **old_info,
                    "build_number": 3,
                    "build_version": old_info["source_version"] + "-3",
                }
            )
        )
        self.assertEqual(self.reserve()["build_number"], 4)
        migrated = json.loads(state.read_text())
        self.assertEqual(migrated["schema_version"], 3)
        self.assertEqual(migrated["legacy_versions"]["1.2.0-fork2.0.1"], 9)
        self.assertEqual(migrated["counters"][old_info["source_commit"]], 5)

    def test_new_commit_resets_build_counter(self):
        self.assertEqual(self.reserve()["build_number"], 0)
        self.assertEqual(self.reserve()["build_number"], 1)
        self.git("commit", "--allow-empty", "-qm", "Another fork commit")
        self.assertEqual(self.reserve()["build_version"], "1.2.0-fork0.2.2-0")

    def test_amend_equivalent_keeps_commit_count_but_resets_builds(self):
        first = self.reserve()
        self.assertEqual(self.reserve()["build_number"], 1)
        # Replace only the fixture's tip using plumbing; never amend a user's commit.
        tree = self.git("rev-parse", "HEAD^{tree}")
        parent = self.git("rev-parse", "HEAD^")
        replacement = self.git(
            "commit-tree", tree, "-p", parent, input="Reworded fixture tip\n"
        )
        self.git("update-ref", "refs/heads/main", replacement)
        second = self.reserve()
        self.assertNotEqual(first["source_commit"], second["source_commit"])
        self.assertEqual(second["source_version"], first["source_version"])
        self.assertEqual(second["build_number"], 0)
        self.assertEqual(self.reserve()["build_number"], 1)

    def test_dirty_rebuilds_keep_the_same_commit_build_sequence(self):
        first = self.reserve()
        (self.root / "pending").write_text("uncommitted source changes\n")
        second = self.reserve()
        self.assertEqual(second["source_commit"], first["source_commit"])
        self.assertEqual(second["source_version"], first["source_version"])
        self.assertTrue(second["dirty"])
        self.assertEqual(second["build_number"], 1)

    def test_returning_to_an_earlier_commit_retains_its_build_sequence(self):
        first = self.reserve()
        tree = self.git("rev-parse", "HEAD^{tree}")
        parent = self.git("rev-parse", "HEAD^")
        replacement = self.git("commit-tree", tree, "-p", parent, input="Other tip\n")
        self.git("update-ref", "refs/heads/main", replacement)
        self.assertEqual(self.reserve()["build_number"], 0)
        self.git("update-ref", "refs/heads/main", first["source_commit"])
        self.assertEqual(self.reserve()["build_number"], 1)

    def test_unattributed_version_only_counts_do_not_leak_into_a_new_commit(self):
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        counter.write_text("4\n")
        state = counter.with_name(counter.name + ".versions.json")
        state.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "counters": {"1.2.0-fork0.2.1": 4},
                }
            )
        )
        self.assertEqual(self.reserve()["build_number"], 0)
        self.assertEqual(
            json.loads(state.read_text())["legacy_versions"],
            {
                "1.2.0-fork0.2.1": 4,
            },
        )

    def test_proven_legacy_labels_for_one_commit_share_a_build_sequence(self):
        info = json.loads(self.command("source").stdout)
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        counter.write_text("4\n")
        state = counter.with_name(counter.name + ".versions.json")
        state.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "counters": {"1.2.0-fork0.2.1": 4, "1.2.0-fork2.0.1": 9},
                }
            )
        )
        output = self.root / "build/stamp"
        output.mkdir()
        for directory, label, number in (
            (output, "1.2.0-fork0.2.1", 3),
            (counter.parent, "1.2.0-fork2.0.1", 8),
        ):
            (directory / "BUILD-VERSION.json").write_text(
                json.dumps(
                    {
                        **info,
                        "source_version": label,
                        "build_number": number,
                        "build_version": f"{label}-{number}",
                    }
                )
            )
        self.assertEqual(self.reserve()["build_number"], 13)

    def test_conflicting_legacy_commit_provenance_is_rejected(self):
        info = json.loads(self.command("source").stdout)
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        counter.write_text("1\n")
        state = counter.with_name(counter.name + ".versions.json")
        original = json.dumps(
            {
                "schema_version": 2,
                "counters": {info["source_version"]: 1},
            }
        )
        state.write_text(original)
        output = self.root / "build/stamp"
        output.mkdir()
        for directory, commit in (
            (output, info["source_commit"]),
            (counter.parent, "a" * 40),
        ):
            (directory / "BUILD-VERSION.json").write_text(
                json.dumps(
                    {
                        **info,
                        "source_commit": commit,
                        "build_number": 0,
                        "build_version": info["source_version"] + "-0",
                    }
                )
            )
        self.assertNotEqual(
            self.command("reserve", "--output", output, check=False).returncode, 0
        )
        self.assertEqual(state.read_text(), original)
        self.assertEqual(counter.read_text(), "1\n")

    def test_corrupt_commit_scoped_state_is_not_silently_reset(self):
        self.reserve()
        state = self.root / "build/build-number.versions.json"
        commit = self.git("rev-parse", "HEAD")
        for counters in ({commit: True}, {"not-a-commit": 1}, {commit: -1}):
            with self.subTest(counters=counters):
                original = json.dumps({"schema_version": 3, "counters": counters})
                state.write_text(original)
                result = self.command(
                    "reserve", "--output", self.root / "build/invalid", check=False
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(state.read_text(), original)
                self.assertFalse((self.root / "build/invalid/BUILD-VERSION").exists())

    def test_explicit_history_bump_does_not_commit_or_move_head(self):
        self.reserve()
        (self.root / "FORK-VERSION").chmod(0o640)
        head = self.git("rev-parse", "HEAD")
        self.command("history")
        self.assertEqual(self.git("rev-parse", "HEAD"), head)
        self.assertEqual((self.root / "FORK-VERSION").read_text(), "1.2.0-fork0.3.0\n")
        self.assertEqual((self.root / "FORK-VERSION").stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.reserve()["build_version"], "1.2.0-fork0.3.1-1")

    def test_parallel_builds_do_not_collide(self):
        with ThreadPoolExecutor(max_workers=8) as pool:
            stamps = list(pool.map(self.reserve, map(str, range(16))))
        self.assertEqual(
            sorted(stamp["build_number"] for stamp in stamps), list(range(16))
        )

    def test_legacy_global_counter_is_preserved_but_not_used_as_revision_count(self):
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        counter.write_text("67\n")
        self.assertEqual(self.reserve()["build_number"], 0)
        state = json.loads(
            counter.with_name(counter.name + ".versions.json").read_text()
        )
        self.assertEqual(state["legacy_next_build"], 67)

    def test_invalid_counters_are_not_silently_reset(self):
        counter = self.root / "build/build-number"
        counter.parent.mkdir()
        for text in ("", "-1", "01", "garbage", "999999999999"):
            counter.write_text(text)
            result = self.command(
                "reserve", "--output", self.root / "build/out", check=False
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(counter.read_text(), text)
            self.assertFalse((self.root / "build/out/BUILD-VERSION").exists())

    def test_corrupt_scoped_state_is_not_silently_reset(self):
        self.reserve()
        state = self.root / "build/build-number.versions.json"
        state.write_text('{"schema_version":2,"counters":{"1.2.0-fork0.2.1":true}}')
        result = self.command(
            "reserve", "--output", self.root / "build/other", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "build/other/BUILD-VERSION").exists())

    def test_text_and_json_exports_have_the_same_source_identity(self):
        stamp = self.reserve()
        exported = self.root.parent / "export"
        exported.mkdir()
        for name in ("FORK-VERSION", "FORK-BASE"):
            shutil.copyfile(self.root / name, exported / name)
        for name in ("SOURCE-VERSION", "SOURCE-COMMIT", "SOURCE-VERSION.json"):
            shutil.copyfile(self.root / "build/stamp" / name, exported / name)
        self.assertEqual(
            versioning.source(exported)["source_commit"], stamp["source_commit"]
        )
        (exported / "SOURCE-COMMIT").write_text("a" * 40)
        with self.assertRaises(ValueError):
            versioning.source(exported)
        (exported / "SOURCE-COMMIT").write_text(stamp["source_commit"])
        (exported / "SOURCE-VERSION.json").unlink()
        self.assertEqual(
            versioning.source(exported)["source_version"], stamp["source_version"]
        )
        (exported / "SOURCE-COMMIT").write_text("a" * 40)
        with self.assertRaises(ValueError):
            versioning.source(exported)

    def test_provenance_formats_remain_private(self):
        (self.root / "pending").write_text("uncommitted change\n")
        stamp = self.reserve()
        self.assertTrue(stamp["dirty"])
        self.assertNotIn("dirty", stamp["build_version"])
        self.assertNotIn("+g", stamp["build_version"])
        self.assertTrue(
            (self.root / "build/stamp/SOURCE-VERSION").read_text().endswith(".dirty\n")
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
