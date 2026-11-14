"""Independent upstream-rebase, history-rewrite, commit and build counters."""

import argparse
from contextlib import contextmanager
from datetime import datetime
from datetime import timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = re.compile(
    r"([0-9][0-9A-Za-z.+-]{0,39})-fork(0|[1-9][0-9]*)"
    r"\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
)
UPSTREAM = re.compile(r"[0-9][0-9A-Za-z.+-]{0,39}")
NUMBER = re.compile(r"0|[1-9][0-9]{0,11}")
COMMIT = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
LIMIT = 999_999_999_999


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *args],
        env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"},
        text=True,
    ).strip()


def compose(upstream: str, rebases: int, rewrites: int, count: int) -> str:
    if UPSTREAM.fullmatch(upstream) is None or any(
        type(value) is not int or value < 0 for value in (rebases, rewrites, count)
    ):
        raise ValueError("upstream version and nonnegative fork counters are required")
    return f"{upstream}-fork{rebases}.{rewrites}.{count}"


def base_version(root: Path):
    text = (root / "FORK-VERSION").read_text().strip()
    match = RELEASE.fullmatch(text)
    if match is None or match[4] != "0":
        raise ValueError("FORK-VERSION must contain UPSTREAM-forkREBASES.REWRITES.0")
    return text, match[1], int(match[2]), int(match[3])


def validate_info(info: dict, upstream: str, rebases: int, rewrites: int):
    match = RELEASE.fullmatch(info["source_version"])
    if (
        match is None
        or match[1] != upstream
        or int(match[2]) != rebases
        or int(match[3]) != rewrites
        or not isinstance(info["source_commit"], str)
        or COMMIT.fullmatch(info["source_commit"]) is None
        or type(info["dirty"]) is not bool
    ):
        raise ValueError("invalid or mismatched exported source provenance")


def provenance(info: dict) -> str:
    return f"{info['source_version']}+g{info['source_commit'][:12]}" + (
        ".dirty" if info["dirty"] else ""
    )


def source(root: Path) -> dict:
    root = root.resolve()
    original, upstream, rebases, rewrites = base_version(root)
    if not (root / ".git").exists():
        if (root / "SOURCE-VERSION.json").exists():
            info = json.loads((root / "SOURCE-VERSION.json").read_text())
            info = {
                key: info[key] for key in ("source_version", "source_commit", "dirty")
            }
            text_files = [root / name for name in ("SOURCE-VERSION", "SOURCE-COMMIT")]
            if any(path.exists() for path in text_files):
                if (
                    text_files[0].read_text().strip() != provenance(info)
                    or text_files[1].read_text().strip() != info["source_commit"]
                ):
                    raise ValueError("text and JSON source provenance disagree")
        else:
            version = (root / "SOURCE-VERSION").read_text().strip()
            release, marker, identity = version.rpartition("+g")
            commit = (root / "SOURCE-COMMIT").read_text().strip()
            if not marker or identity != commit[:12] + (
                ".dirty" if identity.endswith(".dirty") else ""
            ):
                raise ValueError(
                    "source commit provenance disagrees with SOURCE-VERSION"
                )
            info = dict(
                source_version=release,
                source_commit=commit,
                dirty=identity.endswith(".dirty"),
            )
        validate_info(info, upstream, rebases, rewrites)
        return info
    commit = git(root, "rev-parse", "--verify", "HEAD")
    anchor = (root / "FORK-BASE").read_text().strip()
    if COMMIT.fullmatch(anchor) is None:
        raise ValueError("FORK-BASE must contain the full upstream boundary commit ID")
    git(root, "merge-base", "--is-ancestor", anchor, commit)
    count = int(
        git(root, "rev-list", "--first-parent", "--count", f"{anchor}..{commit}")
    )
    version = compose(upstream, rebases, rewrites, count)
    if git(root, "rev-parse", f"{commit}~{count}") != anchor:
        raise ValueError(
            "FORK-BASE must be on the first-parent history, not only a merged ancestor"
        )
    info = dict(
        source_version=version,
        source_commit=commit,
        dirty=bool(git(root, "status", "--porcelain=v1", "--untracked-files=normal")),
    )
    if (
        git(root, "rev-parse", "HEAD") != commit
        or base_version(root)[0] != original
        or (root / "FORK-BASE").read_text().strip() != anchor
    ):
        raise ValueError("source identity changed while determining the version; retry")
    return info


@contextmanager
def counter_lock(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt

            if path.stat().st_size == 0:
                lock.write(b"\0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock, fcntl.LOCK_EX)
            yield


def atomic_write(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", dir=path.parent, delete=False
        ) as stream:
            temporary = Path(stream.name)
            if path.exists() and hasattr(os, "fchmod"):
                os.fchmod(stream.fileno(), path.stat().st_mode & 0o777)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def reserve(root: Path, output: Path, allow_source_output=False) -> dict:
    root, output = root.resolve(), output.resolve()
    info = source(root)
    counter = Path(
        os.environ.get("BUILD_COUNTER_FILE", root / "build/build-number")
    ).resolve()
    if (output == root and not allow_source_output) or counter.parent == root:
        raise ValueError("build metadata must be written into a build directory")
    state_file = counter.with_name(counter.name + ".versions.json")
    with counter_lock(counter.with_name(counter.name + ".lock")):
        previous = counter.read_text().strip() if counter.exists() else "0"
        if NUMBER.fullmatch(previous) is None or int(previous) >= LIMIT:
            raise ValueError("invalid or exhausted build counter")
        if state_file.exists():
            state = json.loads(state_file.read_text())
            if (
                type(state) is not dict
                or state.get("schema_version") not in (2, 3)
                or type(state.get("counters")) is not dict
            ):
                raise ValueError("invalid revision-scoped build counter state")
        else:
            state = dict(schema_version=3, legacy_next_build=int(previous), counters={})
        legacy = (
            state["counters"]
            if state["schema_version"] == 2
            else state.get("legacy_versions", {})
        )
        if type(legacy) is not dict or any(
            RELEASE.fullmatch(key) is None
            or type(value) is not int
            or not 0 <= value <= LIMIT
            for key, value in legacy.items()
        ):
            raise ValueError("invalid legacy version-scoped build counter entry")
        if state["schema_version"] == 2:
            state["schema_version"] = 3
            state["legacy_versions"] = legacy
            state["counters"] = {}
            attributed = {}
            # A version label alone cannot distinguish an amended commit from its predecessor.
            for path in {
                output / "BUILD-VERSION.json",
                counter.parent / "BUILD-VERSION.json",
            }:
                if not path.exists():
                    continue
                stamp = json.loads(path.read_text())
                label, commit, number = (
                    stamp.get("source_version"),
                    stamp.get("source_commit"),
                    stamp.get("build_number"),
                )
                if label not in legacy:
                    continue
                if (
                    not isinstance(commit, str)
                    or COMMIT.fullmatch(commit) is None
                    or type(number) is not int
                    or not 0 <= number < legacy[label]
                    or stamp.get("build_version") != f"{label}-{number}"
                    or label in attributed
                    and attributed[label] != commit
                ):
                    raise ValueError("inconsistent legacy build provenance")
                attributed[label] = commit
            for label, commit in attributed.items():
                state["counters"][commit] = (
                    state["counters"].get(commit, 0) + legacy[label]
                )
        counters = state["counters"]
        for key, value in counters.items():
            if (
                COMMIT.fullmatch(key) is None
                or type(value) is not int
                or not 0 <= value <= LIMIT
            ):
                raise ValueError("invalid revision-scoped build counter entry")
        key = info["source_commit"]
        number = counters.get(key, 0)
        if number >= LIMIT:
            raise ValueError("build counter exhausted for this revision")
        counters[key] = number + 1
        # The mapping is authoritative; the numeric file remains a human-readable latest counter.
        atomic_write(state_file, json.dumps(state, sort_keys=True, indent=2) + "\n")
        atomic_write(counter, f"{number + 1}\n")
    if source(root) != info:
        raise ValueError("source identity changed while reserving a build; retry")
    stamp = {
        **info,
        "build_number": number,
        "build_version": f"{info['source_version']}-{number}",
    }
    for name, value in (
        ("SOURCE-VERSION", provenance(info)),
        ("SOURCE-COMMIT", info["source_commit"]),
        ("BUILD-ID", str(number)),
        ("BUILD-VERSION", stamp["build_version"]),
    ):
        atomic_write(output / name, value + "\n")
    atomic_write(output / "SOURCE-VERSION.json", json.dumps(info, indent=2) + "\n")
    atomic_write(output / "BUILD-VERSION.json", json.dumps(stamp, indent=2) + "\n")
    return stamp


def build_stamp(root: Path, output: Path) -> dict:
    preallocated = os.environ.get("CODEX_BUILD_VERSION_FILE")
    if preallocated is None:
        return reserve(root, output)
    stamp = json.loads(Path(preallocated).read_text())
    number = stamp["build_number"]
    if (
        {key: stamp[key] for key in ("source_version", "source_commit", "dirty")}
        != source(root)
        or type(number) is not int
        or not 0 <= number < LIMIT
        or stamp["build_version"] != f"{stamp['source_version']}-{number}"
    ):
        raise ValueError(
            "preallocated build stamp is invalid or belongs to another source revision"
        )
    return stamp


def record_change(root: Path, kind: str, new_base=None, new_upstream=None) -> str:
    root = root.resolve()
    if not (root / ".git").exists():
        raise ValueError(
            "record fork history changes in the maintained Git repository, not an export"
        )
    lock = Path(git(root, "rev-parse", "--git-path", "fork-history-version.lock"))
    if not lock.is_absolute():
        lock = root / lock
    with counter_lock(lock):
        old_version, upstream, rebases, rewrites = base_version(root)
        old_base = (root / "FORK-BASE").read_text().strip()
        if COMMIT.fullmatch(old_base) is None:
            raise ValueError(
                "FORK-BASE must contain the full upstream boundary commit ID"
            )
        anchor = old_base
        if kind == "rebase":
            if new_base is None or new_upstream is None:
                raise ValueError("rebase requires --base COMMIT and --upstream VERSION")
            anchor = git(
                root,
                "rev-parse",
                "--verify",
                "--end-of-options",
                new_base + "^{commit}",
            )
            if anchor == old_base and new_upstream == upstream:
                raise ValueError(
                    "this upstream version and boundary are already recorded"
                )
            git(root, "merge-base", "--is-ancestor", anchor, "HEAD")
            count = int(
                git(root, "rev-list", "--first-parent", "--count", f"{anchor}..HEAD")
            )
            if git(root, "rev-parse", f"HEAD~{count}") != anchor:
                raise ValueError("the new boundary must be on the first-parent history")
            result = compose(new_upstream, rebases + 1, rewrites, 0)
        elif kind == "history-rewrite" and new_base is None and new_upstream is None:
            result = compose(upstream, rebases, rewrites + 1, 0)
        else:
            raise ValueError(
                "use rebase to change the upstream boundary, or history for a rewrite"
            )
        history_path = root / "FORK-HISTORY.json"
        history = (
            json.loads(history_path.read_text())
            if history_path.exists()
            else {
                "schema_version": 2,
                "initial_rebases": rebases,
                "initial_history_rewrites": rewrites,
                "events": [],
            }
        )
        if not isinstance(history, dict) or not isinstance(
            history.get("events", []), list
        ):
            raise ValueError("invalid fork history ledger; refusing to replace it")
        history["schema_version"] = 2
        history.setdefault("events", []).append(
            {
                "kind": kind,
                "recorded_at": datetime.now(timezone.utc).isoformat(),
                "old_version": old_version,
                "new_version": result,
                "old_base": old_base,
                "new_base": anchor,
                "head": git(root, "rev-parse", "HEAD"),
            }
        )
        if anchor != old_base:
            atomic_write(root / "FORK-BASE", anchor + "\n")
        atomic_write(root / "FORK-VERSION", result + "\n")
        atomic_write(history_path, json.dumps(history, indent=2) + "\n")
    return result


def bump_history(root: Path, new_base=None) -> str:
    return record_change(root, "history-rewrite", new_base=new_base)


def bump_rebase(root: Path, new_base: str, upstream: str) -> str:
    return record_change(root, "rebase", new_base=new_base, new_upstream=upstream)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "operation", choices=("source", "reserve", "cargo", "history", "rebase")
    )
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--format", choices=("json", "provenance", "commit", "build"), default="json"
    )
    parser.add_argument("--allow-source-output", action="store_true")
    parser.add_argument("--base")
    parser.add_argument("--upstream")
    args = parser.parse_args()
    if args.operation == "rebase":
        print(bump_rebase(args.root, args.base, args.upstream))
        return 0
    if args.base is not None or args.upstream is not None:
        parser.error("--base and --upstream belong to the rebase operation")
    if args.operation == "source":
        result = source(args.root)
    elif args.operation == "history":
        print(bump_history(args.root))
        return 0
    elif args.output is None:
        parser.error("reserve and cargo require --output")
    elif args.operation == "reserve":
        result = reserve(args.root, args.output, args.allow_source_output)
    else:
        result = build_stamp(args.root, args.output)
        for name, key in (
            ("CODEX_FORK_BUILD_VERSION", "build_version"),
            ("CODEX_FORK_BUILD_COMMIT", "source_commit"),
        ):
            print(f"cargo:rustc-env={name}={result[key]}")
        return 0
    if args.format == "provenance":
        print(provenance(result))
    elif args.format == "commit":
        print(result["source_commit"])
    elif args.format == "build":
        print(result["build_version"])
    else:
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        subprocess.CalledProcessError,
    ) as error:
        sys.exit(f"Fork versioning failed: {error}")
