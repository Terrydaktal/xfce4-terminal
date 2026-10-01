regex_test = executable(
  'test-terminal-regex',
  'test-terminal-regex.c',
  include_directories: include_directories('..'),
  dependencies: [glib, pcre2],
)

test('terminal-regex', regex_test)

mouse_test = executable(
  'test-terminal-mouse',
  'test-terminal-mouse.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-preferences.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4util, xfconf, libutempter],
)

test('terminal-mouse', mouse_test, timeout: 30)

keys_test = executable(
  'test-terminal-keys',
  'test-terminal-keys.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-preferences.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4util, xfconf, libutempter],
  c_args: ['-DTEST_SOURCE_DIR="' + (meson.current_source_dir() / '..') + '"'],
)

test('terminal-keys', keys_test, timeout: 30, env: {'TERM': 'xterm-256color'})

links_test = executable(
  'test-terminal-links',
  'test-terminal-links.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-preferences.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4util, xfconf, libutempter],
)

copy_test = executable(
  'test-terminal-copy',
  'test-terminal-copy.c',
  '../terminal/terminal-app.c',
  '../terminal/terminal-encoding-action.c',
  '../terminal/terminal-gdbus.c',
  '../terminal/terminal-image-loader.c',
  '../terminal/terminal-options.c',
  '../terminal/terminal-preferences-dialog.c',
  '../terminal/terminal-preferences.c',
  '../terminal/terminal-search-dialog.c',
  '../terminal/terminal-screen.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-util.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-window-dropdown.c',
  '../terminal/terminal-window.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  xfce_revision_h,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [
    glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4kbd, libxfce4util, xfconf,
    x11_deps, wayland_deps, gtk_layer_shell, libutempter, libprocstat, libutil,
  ],
)

# Native clipboard ownership must not race other GUI tests on the same display.
test('terminal-copy', copy_test, timeout: 30, is_parallel: false, env: {'TERM': 'xterm-256color'})
