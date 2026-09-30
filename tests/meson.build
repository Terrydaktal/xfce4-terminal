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
  '../terminal/terminal-preferences.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4util, xfconf, libutempter],
)

test('terminal-mouse', mouse_test, timeout: 30)
