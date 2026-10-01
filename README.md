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

### Local Path Hyperlinks

This fork can detect local file paths in output when the foreground PTY
application is listed by the `misc-auto-detect-file-path-apps` preference. The
default allowlist covers Fish, Codex, Antigravity, Gemini, and AGY. Existing
files open through their normal desktop handler; files that need selection are
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
foreground-process allowlist is evaluated at click time rather than against
the historical producer of a line.

For local tmux clients launched with an explicit absolute `-S` socket (including
tmux-simple), path resolution now follows that client's active pane. A bounded
read-only query to the same tmux binary maps the client PID to its pane; `/proc`
then supplies the pane's foreground process and current working directory.
This preserves the application allowlist and resolves relative paths against
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
recorders. It is driven by `~/Dev/tmux-simple/tests/test_hyperlinks.py` with a
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
