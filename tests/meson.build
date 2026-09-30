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
