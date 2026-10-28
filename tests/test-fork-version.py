"""Private Git/Meson fixtures; never build or restart a user's terminal window."""

import os
import shutil
import subprocess
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ForkVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xfce-fork-version-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.repo = self.directory / "source"
        (self.repo / "scripts").mkdir(parents=True)
        for script in ROOT.joinpath("scripts").glob("*.sh"):
            shutil.copyfile(script, self.repo / "scripts" / script.name)
        shutil.copyfile(ROOT / "xfce-revision.h.in", self.repo / "xfce-revision.h.in")
        (self.repo / "FORK-VERSION").write_text("1.2.0-fork0.0.0\n")
        shutil.copyfile(ROOT / ".gitignore", self.repo / ".gitignore")
        self.environment = os.environ.copy()
        self.environment.pop("BUILD_COUNTER_FILE", None)
        self.environment.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-b", "main")
        self.git("add", ".")
        self.git("commit", "-qm", "Initial fork fixture")
        (self.repo / "FORK-BASE").write_text(self.git("rev-parse", "HEAD").stdout)
        self.git("add", ".")
        self.git("commit", "-qm", "Record fixture baseline")

    def run_command(self, *arguments, check=True):
        return subprocess.run(
            [str(argument) for argument in arguments],
            capture_output=True,
            text=True,
            env=self.environment,
            timeout=30,
            check=check,
        )

    def git(self, *arguments):
        return self.run_command(
            "git",
            "-C",
            self.repo,
            "-c",
            "user.name=Terrydaktal",
            "-c",
            "user.email=9lewis9@gmail.com",
            "-c",
            "commit.gpgsign=false",
            "-c",
            "core.hooksPath=/dev/null",
            *arguments,
        )

    def generate(self, name="out", *, root=None, check=True):
        root = root or self.repo
        header = root / "build" / name / "xfce-revision.h"
        result = self.run_command(
            "bash", root / "scripts/generate-build-header.sh", header, check=check
        )
        return header, result

    def assert_version(self, header, version):
        self.assertIn(f'#define VERSION_FULL "{version}"', header.read_text())
        self.assertEqual((header.parent / "BUILD-VERSION").read_text(), version + "\n")

    def test_rebuilds_increment_without_changing_tracked_sources(self):
        before = self.git("status", "--porcelain=v1").stdout
        for number in range(2):
            header, result = self.generate()
            self.assertEqual(result.stdout, f"1.2.0-fork0.0.2-{number}\n")
            self.assert_version(header, f"1.2.0-fork0.0.2-{number}")
            self.assertEqual((header.parent / "BUILD-ID").read_text(), f"{number}\n")
            self.assertEqual(
                (header.parent / "SOURCE-COMMIT").read_text(),
                self.git("rev-parse", "HEAD").stdout,
            )
        self.assertEqual(self.git("status", "--porcelain=v1").stdout, before)

    def test_commits_advance_release_and_dirty_is_only_diagnostic(self):
        (self.repo / "payload").write_text("Uncommitted fixture source\n")
        header, _ = self.generate()
        self.assert_version(header, "1.2.0-fork0.0.2-0")
        self.assertTrue(
            (header.parent / "SOURCE-VERSION").read_text().endswith(".dirty\n")
        )
        self.git("add", ".")
        self.git("commit", "-qm", "Next fork revision")
        header, _ = self.generate()
        self.assert_version(header, "1.2.0-fork0.0.3-1")

    def test_decimal_carry_and_git_free_export(self):
        cases = [
            ("0.0.7", "0.0.9"),
            ("0.0.8", "0.1.0"),
            ("0.1.6", "0.1.8"),
            ("0.2.0", "0.2.2"),
            ("0.3.9", "0.4.1"),
            ("0.9.8", "1.0.0"),
            ("9.9.8", "10.0.0"),
        ]
        for number, (base, expected) in enumerate(cases):
            with self.subTest(base=base):
                (self.repo / "FORK-VERSION").write_text(f"1.2.0-fork{base}\n")
                header, _ = self.generate()
                self.assert_version(header, f"1.2.0-fork{expected}-{number}")
                exported = self.directory / f"export-{number}"
                self.run_command(
                    "bash", self.repo / "scripts/source-snapshot.sh", exported
                )
                header, _ = self.generate(root=exported)
                self.assert_version(header, f"1.2.0-fork{expected}-0")

    def test_parallel_build_directories_have_distinct_numbers(self):
        with ThreadPoolExecutor(max_workers=8) as workers:
            results = list(
                workers.map(lambda number: self.generate(f"out-{number}"), range(8))
            )
        self.assertEqual(
            {result.stdout.strip() for _, result in results},
            {f"1.2.0-fork0.0.2-{number}" for number in range(8)},
        )
        for header, result in results:
            self.assert_version(header, result.stdout.strip())
        self.assertEqual((self.repo / "build/build-number").read_text(), "8\n")

    def test_bad_counter_does_not_replace_header(self):
        header, _ = self.generate()
        previous = header.read_bytes()
        counter = self.repo / "build/build-number"
        for invalid in ("", "-1", "01", "invalid", "999999999999"):
            with self.subTest(counter=invalid):
                counter.write_text(invalid + "\n")
                _, result = self.generate(check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(header.read_bytes(), previous)
                self.assertEqual(counter.read_text(), invalid + "\n")

    def test_missing_export_provenance_is_not_replaced_by_unknown_version(self):
        exported = self.directory / "export"
        self.run_command("bash", self.repo / "scripts/source-snapshot.sh", exported)
        (exported / "SOURCE-COMMIT").unlink()
        header, result = self.generate(root=exported, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(header.exists())

    def test_manual_bumps_carry(self):
        for before, part, after in [
            ("0.0.9", "patch", "0.1.0"),
            ("0.9.9", "patch", "1.0.0"),
            ("0.9.8", "minor", "1.0.0"),
        ]:
            with self.subTest(before=before, part=part):
                (self.repo / "FORK-VERSION").write_text(f"1.2.0-fork{before}\n")
                result = self.run_command(
                    "bash", self.repo / "scripts/bump-version.sh", part
                )
                self.assertEqual(result.stdout, f"1.2.0-fork{after}\n")

    def test_normal_meson_rebuilds_get_new_versions_without_reconfigure(self):
        if not shutil.which("meson") or not shutil.which("ninja"):
            self.skipTest("Meson and Ninja are needed for the incremental build check")
        (self.repo / "meson.build").write_text(
            "project('version-probe', 'c')\n"
            "header = custom_target('fork-build-version', output: 'xfce-revision.h',\n"
            "  command: ['bash', meson.current_source_dir() / 'scripts/generate-build-header.sh', '@OUTPUT@'],\n"
            "  build_always_stale: true)\n"
            "executable('version-probe', 'probe.c', header, include_directories: include_directories('.'))\n"
        )
        (self.repo / "probe.c").write_text(
            '#include <stdio.h>\n#include "xfce-revision.h"\n'
            "int main(void) { puts(VERSION_FULL); return 0; }\n"
        )
        build = self.repo / "build/native"
        self.run_command("meson", "setup", build, self.repo)
        for number in range(2):
            self.run_command("meson", "compile", "-C", build)
            result = self.run_command(build / "version-probe")
            self.assertEqual(result.stdout, f"1.2.0-fork0.0.2-{number}\n")
        self.run_command("ninja", "-C", build, "-n")
        self.assertEqual((self.repo / "build/build-number").read_text(), "2\n")
        self.git("add", ".")
        self.git("commit", "-qm", "Record Meson fixture")
        self.run_command("meson", "compile", "-C", build)
        self.assertEqual(
            self.run_command(build / "version-probe").stdout, "1.2.0-fork0.0.3-2\n"
        )


if __name__ == "__main__":
    unittest.main()
