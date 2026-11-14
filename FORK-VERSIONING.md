# Fork Versioning

The public version is `UPSTREAM-forkREBASES.HISTORY_REWRITES.COMMITS-BUILD`,
for example `1.2.0-fork1.2.57-0`.
These are independent history/build counters, not semantic API versions.

## Rules

- `REBASES` counts completed moves/ports of our patches to a new upstream base.
  Cloning the initial base is zero. An upstream upgrade increments only this
  counter, retaining the cumulative history-rewrite count.
- `HISTORY_REWRITES` counts deliberate large reconstructions of our fork patch
  history. Amendments, ordinary commits, partial rebases and routine force pushes
  do not automatically increment either history counter.
- `COMMITS` is the full first-parent fork commit count after the excluded
  upstream `FORK-BASE`. There is no base-100 split or application-defined numeric
  ceiling: 99 is followed by 100, and 99,999 is followed by 100,000. Commits never
  carry into the rebase or history-rewrite counter.
- `FORK-VERSION` stores the upstream version and the two explicit counters as
  `1.2.0-fork1.2.0`. The final zero is a placeholder; Git supplies the actual count.
- `BUILD` starts at zero for each full source commit hash. Rebuilds and
  retries advance it; amending HEAD starts a new build sequence without changing
  the commit-count component. Dirty edits and manual counter/label corrections
  do not reset the sequence while HEAD is unchanged. Parallel build directories
  share the same locked per-commit counter state. Cached no-op builds do not
  allocate another number.
- Full commit hashes, dirty provenance and GNU ELF build IDs retain exact
  diagnostic identities separately from the public label. Upstream dependency,
  package and ABI versions are not changed by this fork-numbering scheme.

## Historical Seeds and Ledger

`FORK-HISTORY.json` records initial rebase count 1, initial
history-rewrite count 2, retained push/port/rebase
observations, and subsequent explicitly recorded events. Historical counts are
best-effort from available local evidence, not guaranteed lifetime totals. A
zero rebase seed means no qualifying retained record was found. Builds never
rescan reflogs or silently change these seeds. The reviewed classifications,
excluded amendments/partial rewrites and evidence limitations are stored in the
ledger; duplicate push and local-reconstruction records count only once.

## Recording Changes

Rebase and whole-history-rewrite counters are deliberately manual. No Git hooks
or push/rebase heuristics update them. Ask the maintainer/agent performing the
rare operation to record it explicitly. A force push alone is not a versioning
event. Tip amendments and last-two-commit/suffix rewrites are excluded unless a
deliberate reconstruction actually replaces the entire fork stack. Commit and
build counters remain automatic; never edit the derived commit count by hand.

After actually porting our changes onto an upstream base that is on HEAD's
first-parent history, record the completed upstream upgrade once:

```sh
python3 scripts/fork_version.py rebase --base UPSTREAM_COMMIT --upstream UPSTREAM_VERSION
```

After deliberately reconstructing the fork's whole patch history, record that
operation separately:

```sh
python3 scripts/fork_version.py history
```

The operations update `FORK-VERSION`, the upstream boundary for a rebase, and the
tracked history ledger. They never run Git rebase, rewrite commits, move HEAD,
stage, commit or push. Re-recording the identical upstream version/base is
rejected. Use `rebase`, not `history --base`, to change the upstream boundary.
Shell adapters also accept `bump-version.sh history` or
`bump-version.sh rebase --base COMMIT --upstream VERSION` where supplied.

## Scripts and State

- `python3 scripts/fork_version.py source`: read-only JSON source identity.
- `python3 scripts/fork_version.py reserve --output DIRECTORY`: atomically
  reserve one build and write text/JSON source and build metadata. Only Python
  3's standard library is required; no runtime Python packages are introduced.
- `build/build-number.versions.json` schema 3 stores persistent, locked
  counters keyed by full source commit hashes. `build/build-number` remains the
  latest next-build number. The old cumulative value remains `legacy_next_build`;
  schema-2 label entries are retained in `legacy_versions`. Existing build stamps
  allow proven old reservations to be attributed to their actual commit.
  Unattributable old labels are preserved, not assigned to an unrelated commit.
  `BUILD_COUNTER_FILE` selects a different counter and adjacent state/lock files.
  Do not delete this state if numbering must survive build-directory cleanups.
- Git-free exports require matching `SOURCE-VERSION`/`SOURCE-COMMIT`, or JSON
  source provenance, and retain both explicit history counters. Missing or
  mismatched provenance fails closed.

## Verification

`python3 tests/test-history-versioning.py` tests independent rebase/rewrite counters, arbitrarily
large commit values, real Git commit 100, duplicate/invalid upgrade rejection,
ledger validation, new-commit reset, amendment reset, dirty rebuild continuity, existing-counter
migration, parallel allocation and Git-free source provenance. Fixtures never
change a real branch or restart an application.
