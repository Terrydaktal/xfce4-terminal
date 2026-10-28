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
