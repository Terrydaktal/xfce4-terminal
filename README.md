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
opened through `pcmanfm`.

VTE does not retain the process that originally emitted screen content, so the
foreground-process allowlist is evaluated at click time rather than against
the historical producer of a line.

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
