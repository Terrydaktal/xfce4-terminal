[![License](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://gitlab.xfce.org/apps/xfce4-terminal/-/blob/master/COPYING)

# xfce4-terminal

Xfce Terminal is a lightweight and easy to use terminal emulator application
with many advanced features including drop down, tabs, unlimited scrolling,
full colors, fonts, transparent backgrounds, and more.

----

### Homepage

[Xfce4-terminal documentation](https://docs.xfce.org/apps/xfce4-terminal/start)

### Changelog

See [NEWS](https://gitlab.xfce.org/apps/xfce4-terminal/-/blob/master/NEWS) for details on changes and fixes made in the current release.

### Performance Issues

Xfce Terminal is based on the Vte terminal widget library, just like
gnome-terminal. Vte is probably not the fastest terminal emulation library on
earth, but it's one of the best when it comes to Unicode support, and not to
forget, it's actively developed.

### Selecting Text During Live Output

The default build includes a private, pinned VTE 0.84.1 with a repair for text
selection during streaming output. Upstream compared an unfinished mouse drag
with the previous PRIMARY clipboard selection, so unrelated output could cancel
the drag. The repair compares the active selection immediately before and after
each input batch instead. Output continues while dragging; changes to the
selected text still invalidate the selection. Clipboard ownership changes when
the gesture finishes, as before.

`subprojects/vte.wrap` pins the official source archive and checksum.
`subprojects/packagefiles/vte/` contains the selection repair and private build
integration. Meson fetches and patches the source during setup; downloaded
sources and build products are ignored by Git. Build with `meson setup build`
and `meson compile -C build`. An existing build can use
`meson setup --reconfigure build -Dpatched-vte=true`.

The build-tree executable loads its matching library using Meson's build RPATH.
Installation places the library under `libdir/xfce4-terminal`, with an
executable-scoped `$ORIGIN` RUNPATH. No `LD_LIBRARY_PATH` is exported into child
shells, and the system VTE is unchanged. The executable requires a marker symbol
from the patched library, so a missing private library cannot silently fall back
to the unpatched system copy. `-Dpatched-vte=false` explicitly selects system VTE
and omits this repair. Existing terminal processes retain their loaded library
until they are restarted.

`meson test -C build --no-rebuild terminal-stream-selection` exercises real PTY
output and mouse dragging on a private Xvfb display. It checks wheel scrolling,
autoscroll above the viewport, continued output, clipboard contents on release,
and selection invalidation when the selected text actually changes. The test
requires Python with PyGObject, `xvfb-run` and `xdotool`; it uses synthetic text
and never sends input to an existing terminal window.

### Launching

**Preferences -> Advanced -> Launching** contains **Start each terminal launch in
a separate process**. The `/misc-run-in-separate-process` boolean in the
`xfce4-terminal` Xfconf channel applies before launch forwarding, whether the
executable is started by a desktop launcher, shortcut, shell or another program.
It affects future invocations only, not windows or tabs already open. The
upstream default remains process reuse (the checkbox is off).

`--disable-server` forces a separate process; `--enable-server` requests reuse of
an existing launch server, starting a new server if none exists. Both override
the saved preference, and the last flag wins when both are supplied. Arguments
to a child command after `--execute`/`-x` or `--` are not terminal overrides.
Separate processes still expose their instance D-Bus control API, including
`ListTerminals()` and `SendEnter()`.

The repository wrapper no longer forces `--disable-server`, so the checkbox is
effective for wrapper launches too. It still supplies the launch-path metadata
used below; direct executable launches do not require that metadata.

### Fork Versions

The terminal's `--version`, About dialog and `tmux-mosh clients` use versions such
as `1.2.0-fork0.4.1-0`, with no Git hash or dirty marker in the public version.
`FORK-VERSION` holds the manual base, `1.2.0-fork0.0.0`; `FORK-BASE` pins the first
fork commit. First-parent fork commits count inclusively, excluding older or
merged upstream history. Treat `X.Y.Z` as `100*X + 10*Y + Z`, add the commit count,
and carry minor/patch digits at ten. The 41 fork commits at this migration therefore
produce `0.4.1`; `0.0.9` becomes `0.1.0`, and `0.9.9` becomes `1.0.0`.

Normal `meson compile -C build` builds generate a new numeric suffix, starting at
`-0`, then `-1`, `-2`, and so on, even when the source is unchanged. The ignored
`build/build-number` counter is locked and updated atomically. Separate build
directories share this repository counter; `BUILD_COUNTER_FILE=/absolute/path`
can select another persistent counter. Failed attempts can leave gaps. A new
commit advances the fork revision without resetting the build counter; Meson
does not need manual reconfiguration to see it. `meson setup` and dry runs do not
allocate build numbers. Use `meson test -C build --no-rebuild` to test the existing
artifact without creating another build.

`scripts/source-version.sh` reads Git history or exported provenance;
`scripts/build-version.sh` reserves a build number and writes `BUILD-VERSION`,
`BUILD-ID`, `SOURCE-VERSION` and `SOURCE-COMMIT` in the selected build directory.
`scripts/generate-build-header.sh` turns those values and `xfce-revision.h.in`
into the generated version header. Meson runs it once per terminal build, then
recompiles the version consumers. The upstream Meson project version remains the
compatibility base; `VERSION_FULL` is the public compiled fork/build version.
The full commit and diagnostic dirty state remain in source metadata, not the
displayed version. GNU ELF build IDs remain available for outdated-build checks.

`bash scripts/bump-version.sh [patch|minor|major]` deliberately advances the manual
base with the same carry rules; normal builds do not edit it. For a Git-free
source export, run `bash scripts/source-snapshot.sh DESTINATION` before packaging.
That preserves the source file manifest, input checksums and revision provenance;
the exported source generates its own build numbers. Build scripts never commit,
push, change launcher links or restart existing terminal windows.

`python3 tests/test-fork-version.py` checks numbering, carry, parallel counters,
failure handling, source exports and actual incremental Meson builds in private
temporary fixtures. The same suite is registered as Meson's `fork-versioning` test.

### Outdated Running Builds

When the local executable that would be launched next has a different GNU ELF
build ID from this running process, the window title strikes through only its
`Terminal` label using Unicode combining strokes. Fresh titles are unchanged.
If a shell or application supplies a title without that label, a struck-through
`Terminal` suffix is added while outdated; application, project and tab titles
are otherwise preserved. The indicator clears if the original build is restored.

One shared checker per process monitors the executable and launch symlink,
debounces relevant filesystem changes for one second, and performs a fallback
check every 30 seconds. Candidate identity checks run in a background worker;
unchanged files reuse the cached identity. Nothing is checked during terminal
output, typing or hover, and no network/version lookup or candidate execution is
performed. Missing, unreadable, partial or unsupported executables leave the
last known state unchanged; checking requires GNU ELF build IDs and Linux's
`/proc/self/exe`. Rendering of the combining strokes depends on the titlebar font.

The repository launcher retains its unresolved entry path so repointing
`~/.local/bin/xfce4-terminal` to another repository build is detected too. Its
private `XFCE4_TERMINAL_LAUNCH_PATH` and `XFCE4_TERMINAL_LAUNCH_KIND` variables
are consumed before GTK starts and are not passed to the shell. Opening a window
with this rebuilt version enables the checker; already-running older binaries
cannot acquire it without restarting.

### Application Build Indicators

The terminal also tracks the active tab's local native application, including
Codex without any Codex-side title/build-monitor patch. When its running GNU ELF
build ID differs from the current executable at its launch entry, only the
application label is struck through. If that label is absent, a struck-through
application label is prepended; unrelated title text and the raw OSC/tab title
are preserved. The application's marker and the terminal's own `Terminal` marker
are independent. Legacy application-generated strokes on an explicit app label
are normalized rather than doubled, and removed once the application is known
to match the current local build.

Each window checks its active foreground process every two seconds in a worker,
not during typing, drawing, output or hover. The resolver follows local tmux's
active pane and bounded shell/runtime wrappers to a unique native application.
PID start time and executable inode guard against process reuse and same-PID
`exec`; switching tabs invalidates pending checks immediately. A stable process
reuses its captured running build ID and launch entry. The existing one-second
filesystem debounce and 30-second fallback monitor changes to that entry.

Launch checking uses the application's own executable/arguments and, where it
identifies that same executable, its own PATH. It never chooses an unrelated
installation from the terminal's PATH, executes `--version`, parses arbitrary
wrapper scripts or contacts a server. Absolute native launch entries and
identified launch symlinks are retained across replacements. Remote SSH/Mosh,
ambiguous wrapper children, interpreted applications without an identifiable
native child, and images without GNU ELF build IDs remain unmarked. This is a
comparison with the current local launch target, not an upstream update check;
it does not restart, update or send input to the application.

### Update in Place

Right-click anywhere in a normal terminal window and choose **Update in Place**
to replace that whole window with the current local launch target. The action
is greyed out when the target matches the running build, has not been checked
yet, or is missing, incomplete, unreadable or not executable. An open menu tracks
the build monitor's background availability checks; no disk reads or version
commands run on the GTK thread. The stale title can remain marked while an
update target is temporarily unavailable, but the restart action stays disabled.

The terminal owns the entire operation. It starts its bundled, short-lived
`terminal/update/terminal-update-helper.py` worker in an independent systemd user
scope. No Application Launcher process, service, script, database or configuration
is needed. The helper requires system Python 3.11+, PyGObject/Gio and KWin on the
session bus. A temporary KWin script captures and restores this window's placement
and supported state; it installs no persistent script or shortcut. Drop-down
terminals and windows whose saved normal geometry cannot be recovered are refused.

Every tmux tab reattaches to its exact surviving server/session. The replacement
is verified before the original closes, so programs inside tmux stay running.
For non-tmux or mixed windows, a confirmation explains that other programs will
restart/resume and terminal scrollback, shell variables and unsaved/in-flight
work will be lost. Supported replay includes Fish/Bash/Zsh shells, htop, nvtop,
Antigravity continuation and Codex's exact recoverable session ID. Arbitrary
programs, SSH jobs, incomplete metadata and ambiguous window identities are
refused rather than guessed. An ordinary app cannot have its process memory
transferred into a new terminal.

The helper saves private recovery commands and placement metadata under
`$XDG_STATE_HOME/xfce4-terminal/updates/` (normally
`~/.local/state/xfce4-terminal/updates/`). Each JSON backup lists the executable,
tab working directories and literal application argument arrays; logs and final
`.status.json` reports are stored beside it. If resume fails after the original
closes, the helper reports that failure in its own dialog. Existing Application
Launcher snapshots are neither required nor deleted.
The replacement starts minimized. Its binary, tab identities, startup gates and
placement must verify before the original closes. Non-tmux programs cannot execute
until their original processes have exited; tmux reattaches to the exact live
session first. No process is force-killed. Failures report the actual mismatch,
including early process exit and its error output, instead of only a generic timeout.
An unchanged monitor preserves exact coordinates, even beyond a screen edge.
Missing/changed monitor metadata, tab/process changes or a retargeted launch entry
invalidate the operation. This is restart/resume, not migration of process memory.

Preparation and progress checks run off the GTK thread and only after invoking
the action. Duplicate clicks do not start duplicate updates. The request
is bound to this process's lifetime window UUID and a private helper bus owner.
Only that helper can authorize closing this window. Replacements preserve the launch
entry metadata, including a symlink to the repository wrapper, for future build
checks and updates. This does not fetch Git updates or build the source: rebuild
first with `meson compile -C build`.

The terminal's existing D-Bus interface adds `GetUpdateStatus(window_uuid)` and
`UpdateInPlace(window_uuid)`. The latter returns a terminal-owned update-service
name; clients may observe its `GetStatus()` result but cannot bypass the terminal's
non-tmux confirmation. Application Launcher may use these APIs as an optional
bulk-request client; it no longer owns replacement or placement. Old binaries lack
this protocol and need one manual restart after this migration. Later updates use
the terminal-owned workflow. Isolated GTK tests use a private bus and Xvfb; helper
tests use fake placement backends and real disposable startup gates. They do not
replace the user's desktop windows.

### Local Path Hyperlinks

This fork can detect local file paths in output when the foreground PTY
application is permitted by the `misc-auto-detect-file-path-apps` preference.
The default is `*` (all applications), including Fish and Pi. The separate
`misc-auto-detect-file-path-excluded-apps` preference excludes Nano, Vi/Vim
(including View and restricted variants), Neovim, Emacs and Emacsclient by
default. Both lists are editable in Preferences beside automatic path detection;
use an empty exclusion list to permit every application, or replace `*` with
semicolon-separated names for a stricter allowlist. Explicit OSC 8 links are
unaffected by these lists. Existing files open through their normal desktop
handler; files that need selection are
opened through the executable named by `misc-hyperlink-file-manager` (normally
`pcmanfm`). Missing paths use the optional `unearth` fallback asynchronously.

The fallback accepts hidden and extensionless names, uses a case-sensitive
search, preserves non-UTF-8 filename bytes through Unearth's lossless `%XX`
transport, and ignores stale results from a superseded click. Searches are
bounded by a two-second Unearth timeout rather than an arbitrary result limit.
Python test identifiers in the conventional
`package.module.Class.test_method` form can resolve to their module file.

The matching path requires an Unearth build that supports `--case-sensitive`
and `--lossless-paths`; the forked Unearth source in `/home/lewis/Dev/fsx`
provides these options.

VTE does not retain the process that originally emitted screen content, so the
foreground-process policy is evaluated at click time rather than against
the historical producer of a line. The check includes processes in the same
foreground job, so Bash-wrapped agents work without allowing every Bash job
in a strict allowlist. Any excluded member overrides an allowed wrapper.
Background jobs and other tmux panes cannot allow or exclude the active job.
The process-group traversal has a 50 ms fail-closed budget and does not run
during output, ordinary typing or pointer movement. VTE's regex underlining
is independent of this click-time policy.

Local paths can continue across rows after a slash or a hyphen in a slashed
path, including indented continuations. A row beginning with a complete Unix
permission token such as `drwxr-xr-x` is treated as a separate listing entry,
not a continuation of the preceding directory. This also applies to displayed
`file://` and other URI matches, without changing explicit OSC 8 targets.
Relative directory labels such as `.debug/` are recognized on their own.
The boundary guard uses only the displayed text, without filesystem or process
lookups during hover.

For local tmux clients launched with an explicit absolute `-S` socket (including
tmux-simple), path resolution now follows that client's active pane. A bounded
read-only query to the same tmux binary maps the client PID to its pane; `/proc`
then supplies the pane's foreground process and current working directory.
This preserves the application policy and resolves relative paths against
the program's directory rather than the tmux attach command's old directory.
Queries happen only during a path action, with a 250 ms timeout. Missing or
unresponsive sockets fail closed; no server is created and no shell is invoked.
This does not guess a remote SSH/Mosh machine's filesystem or resolve clients
using implicit/default tmux sockets.

Explicit OSC 8 links also need tmux's `hyperlinks` terminal feature. tmux-simple
enables it for desktop attachments. After updating this fork, open a new
terminal window and reattach to the existing session; the session's programs
do not need restarting. Ctrl+click opens the file; Ctrl+Shift+click retains the
existing parent-directory-and-selection action. Mouse reporting remains enabled
for normal application clicks and scrolling.

The `test-terminal-links` build target exercises those actions through an actual
tmux-simple server, a real widget, and temporary application/file-manager
recorders. It is driven by `~/repos/tmux/tests/test_hyperlinks.py` with a
private HOME/XDG tree, Xvfb and D-Bus; it must not run against the real desktop.

Double-clicking an OSC 8 hyperlink or an automatically detected link selects its
whole displayed span, including spaces, parentheses, Unicode text, and wrapping
across visible rows. Selection follows the label's boundaries rather than the
target URI's punctuation, so `/trash/(agy)name+archive` is not split after `(agy)`.
Quoted paths detected by the regex retain their enclosing quotes in the selection.
Normal word-separator settings are left unchanged. Boundary checks run only on
double-click; they do not add filesystem checks or output/hover processing.

### Mouse Selection With tmux

General > Clipboard has a "Prefer text selection when applications capture the
mouse" option (`misc-prefer-mouse-selection`, disabled by default). When enabled,
ordinary left dragging makes a terminal selection that can be copied with the
configured Copy shortcut. Keep tmux's `mouse on`: wheel events still reach tmux
and browse its history. Shift-left dragging sends mouse input to the application
instead. Existing hyperlink shortcuts take priority over selection handling.

This selects text currently displayed by tmux; selections spanning multiple pages
of tmux's history still require tmux copy mode. It does not change Termux's Android
keyboard or touch handling.

### Right-Click Menus With tmux

At a shell or another non-mouse-aware pane, ordinary right-click opens XFCE's
configured context menu, including Preferences and Update in Place. It also works
while reading or selecting tmux history, without clearing the selection or moving
the viewport. Mouse-aware applications retain right-click input; Shift+right-click
opens the terminal menu instead, just as outside tmux.

The terminal checks the active pane of its attached local tmux client, using the
existing owner-checked explicit `-S` socket and 250 ms query limit. Only right-click
performs this query; ordinary clicks, scrolling and pointer movement do not. If the
pane cannot be verified, normal VTE handling remains in force and Shift+right-click
is still available. Remote SSH/Mosh tmux sessions are not guessed from local state.
The entire locally handled press/motion/release stays local, so no orphan mouse
release reaches the application. No tmux configuration or server restart is needed;
open a window using the rebuilt terminal to activate the change.

`test-terminal-context-menu` uses a real widget and private tmux/PTY fixtures to
check shell, application, Shift and history-selection routing under Xvfb.

### Copy Shortcuts And Application Selections

The Copy accelerator consumes the key only when there is text to copy locally.
A native VTE selection is copied first. For a local tmux client with an explicit,
owned `-S` socket, an existing history selection is copied using
`copy-pipe-no-clear`, targeting that client and pane. This preserves both its
selection and viewport, without sending Ctrl+C to the program. The socket lookup
and copy request are each bounded to 250 ms and run only on Copy, not on ordinary
typing or mouse movement. A failed local tmux query consumes Copy rather than
risk interrupting a program whose selection ownership could not be checked.

Otherwise the original shortcut reaches the foreground application. This matters
when Copy is bound to Ctrl+C: Codex draws its own selection, so VTE reports no
selection. Consuming the accelerator in that case used to prevent Codex's copy
handler from running. Without a selection, Ctrl+C can still interrupt the program.
Forwarding the shortcut does not itself provide an application clipboard backend
or add OSC52 support to VTE. A terminal-owned Shift+drag selection can always be
copied locally, including over SSH.

`test-terminal-copy` exercises the actual window accelerator and PTY, including
application highlighting, native selection precedence, and an isolated tmux
server. Run GUI tests under Xvfb, serially to avoid shared clipboard ownership:

```sh
xvfb-run -a meson test -C build --num-processes 1 --print-errorlogs
```

The installed launcher disables XFCE's shared server. Open a new terminal window
after rebuilding and reattach the existing tmux session; the running tmux server
and its jobs do not need restarting for this shortcut change. Application clipboard
delivery is separate: tmux-simple's application-clipboard patch requires a freshly
started patched tmux server; reattaching cannot upgrade a running old server.

### Modified Enter With tmux

When tmux owns the terminal's foreground process group, Shift+Enter, Ctrl+Enter
and Alt+Enter are sent as distinct CSI-u keys instead of being collapsed to Enter.
Add this to `~/.tmux.conf`, adjusting the repository path if necessary:

```tmux
source-file "$HOME/repos/xfce4-terminal/contrib/tmux-modified-enter.conf"
```

`contrib/tmux-modified-enter.conf` maps these keys to Ctrl+J only when tmux's active
pane command is `codex`, `gemini`, `agy`, or a hyphen-suffixed version of those
names. Other applications receive the original key using their normal tmux key
encoding. Ordinary Enter is not rebound. This uses tmux's own pane metadata, not
per-key shell commands or guesses based on window titles. An interpreter reported
only as `node` is deliberately not treated as an AI CLI.

Load the fragment into an existing server, then attach from a newly opened
patched terminal window:

```sh
tmux source-file ~/repos/xfce4-terminal/contrib/tmux-modified-enter.conf
```

Existing tmux sessions do not need restarting.
No global extended-key or mouse options need changing. Without tmux, the existing
direct Codex/Gemini/agy newline mapping is unchanged.

### Source Code Repository

[Xfce4-terminal source code](https://gitlab.xfce.org/apps/xfce4-terminal)

### Download a Release Tarball

[Xfce4-terminal archive](https://archive.xfce.org/src/apps/xfce4-terminal)
    or
[Xfce4-terminal tags](https://gitlab.xfce.org/apps/xfce4-terminal/-/tags)

### Installation

From source code repository: 

    % cd xfce4-terminal
    % meson setup build
    % meson compile -C build
    % meson install -C build

From release tarball:

    % tar xf xfce4-terminal-<version>.tar.xz
    % cd xfce4-terminal-<version>
    % meson setup build
    % meson compile -C build
    % meson install -C build

### Uninstallation

    % ninja uninstall -C build

### Reporting Bugs

Visit the [reporting bugs](https://docs.xfce.org/apps/xfce4-terminal/bugs) page to view currently open bug reports and instructions on reporting new bugs or submitting bugfixes.
