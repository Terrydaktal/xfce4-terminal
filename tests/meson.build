regex_test = executable(
  'test-terminal-regex',
  'test-terminal-regex.c',
  include_directories: include_directories('..'),
  dependencies: [glib, pcre2],
)

test('terminal-regex', regex_test)
