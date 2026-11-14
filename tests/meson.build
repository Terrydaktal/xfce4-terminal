regex_test = executable(
  'test-terminal-regex',
  'test-terminal-regex.c',
  include_directories: include_directories('..'),
  dependencies: [glib, pcre2],
)

test('terminal-regex', regex_test)

build_monitor_test = executable(
  'test-terminal-build-monitor',
  'test-terminal-build-monitor.c',
  '../terminal/terminal-build-id.c',
  '../terminal/terminal-build-monitor.c',
  include_directories: include_directories('..'),
  dependencies: [glib, gio],
)

test('terminal-build-monitor', build_monitor_test, timeout: 90, env: {'GIO_USE_VFS': 'local'})

freshness_test = executable('test-terminal-freshness-client',
  'test-terminal-freshness-client.c', '../terminal/terminal-freshness-client.c',
  include_directories: include_directories('..'), dependencies: [glib, gio, json_glib])
test('terminal-freshness-client', freshness_test, timeout: 30)

application_fixture = executable('application-monitor-child',
  'fixtures/application-monitor-child.c', c_args: ['-DFIXTURE_VERSION=1'], install: false)
application_fixture_next = executable('application-monitor-child-next',
  'fixtures/application-monitor-child.c', c_args: ['-DFIXTURE_VERSION=2'], install: false)
application_monitor_test = executable('test-terminal-application-monitor',
  'test-terminal-application-monitor.c',
  '../terminal/terminal-application-monitor.c',
  '../terminal/terminal-freshness-client.c',
  '../terminal/terminal-build-monitor.c',
  '../terminal/terminal-build-id.c',
  '../terminal/terminal-tmux.c',
  c_args: ['-DAPPLICATION_FIXTURE="' + application_fixture.full_path() + '"',
           '-DTEST_FRESHNESS_SERVICE="' + (meson.current_source_dir() / 'fixtures/freshness_service.py') + '"',
           '-DAPPLICATION_FIXTURE_NEXT="' + application_fixture_next.full_path() + '"'],
  include_directories: include_directories('..'), dependencies: [glib, gio, json_glib])
test('terminal-application-monitor', application_monitor_test,
  depends: [application_fixture, application_fixture_next], timeout: 60,
  env: {'GIO_USE_VFS': 'local'})

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

context_menu_test = executable(
  'test-terminal-context-menu',
  'test-terminal-context-menu.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-preferences.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [glib, gio, gtk, vte, pcre2, libxfce4ui, libxfce4util, xfconf, libutempter],
)

test('terminal-context-menu', context_menu_test, timeout: 30,
  env: {'TERM': 'xterm-256color'})

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
  'test-terminal-launching.c',
  'test-terminal-update.c',
  '../terminal/terminal-app.c',
  '../terminal/terminal-application-monitor.c',
  '../terminal/terminal-freshness-client.c',
  '../terminal/terminal-build-id.c',
  '../terminal/terminal-build-monitor.c',
  '../terminal/terminal-encoding-action.c',
  '../terminal/terminal-gdbus.c',
  '../terminal/terminal-image-loader.c',
  '../terminal/terminal-options.c',
  '../terminal/terminal-preferences-dialog.c',
  '../terminal/terminal-preferences.c',
  '../terminal/terminal-search-dialog.c',
  '../terminal/terminal-screen.c',
  '../terminal/terminal-restart.c',
  '../terminal/terminal-tmux.c',
  '../terminal/terminal-util.c',
  '../terminal/terminal-update.c',
  '../terminal/terminal-widget.c',
  '../terminal/terminal-window-dropdown.c',
  '../terminal/terminal-window.c',
  terminal_marshal_sources,
  terminal_enum_header,
  terminal_enum_source,
  xfce_revision_h[0],
  c_args: ['-DTEST_TERMINAL_BINARY="' + (meson.current_build_dir() / '../terminal/xfce4-terminal') + '"',
           '-DTEST_FRESHNESS_SERVICE="' + (meson.current_source_dir() / 'fixtures/freshness_service.py') + '"',
           '-DAPPLICATION_FIXTURE="' + application_fixture.full_path() + '"',
           '-DAPPLICATION_FIXTURE_NEXT="' + application_fixture_next.full_path() + '"',
           '-DTEST_UPDATE_WORKER="' + (meson.current_source_dir() / 'fixtures/update_worker.py') + '"'],
  include_directories: [include_directories('..'), include_directories('../terminal')],
  dependencies: [
    glib, gio, json_glib, gtk, vte, pcre2, libxfce4ui, libxfce4kbd, libxfce4util, xfconf,
    x11_deps, wayland_deps, gtk_layer_shell, libutempter, libprocstat, libutil,
  ],
)

# Native clipboard ownership must not race other GUI tests on the same display.
test('terminal-copy', copy_test, depends: [terminal_executable, application_fixture, application_fixture_next],
  timeout: 75, is_parallel: false, env: {'TERM': 'xterm-256color'})

test('fork-versioning', find_program('python3'),
  args: files('test-fork-version.py'), timeout: 120)

test('terminal-update-helper', find_program('python3'),
  args: files('test-terminal-update-helper.py'), timeout: 30)

if get_option('patched-vte')
  selection_test_env = environment()
  selection_test_env.prepend('LD_LIBRARY_PATH',
    meson.project_build_root() / 'subprojects/vte-0.84.1/src')
  test('terminal-stream-selection', find_program('python3'),
    args: files('test-terminal-stream-selection.py'), timeout: 35,
    depends: vte_project.get_variable('libvte_gtk3'),
    env: selection_test_env, is_parallel: false)
endif

update_confirm_module = shared_module('terminal-update-confirm', 'update-confirm-module.c',
  dependencies: gtk, install: false)
kwin_test = find_program('kwin_wayland', required: false)
if kwin_test.found()
  test('terminal-update-wayland', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), terminal_executable.full_path()],
    depends: terminal_executable, timeout: 120, is_parallel: false)
  test('terminal-update-wayland-shell', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--shell', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-titles', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--titles', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module, application_fixture], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-codex', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--codex', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-late-application-exit', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--codex', '--late-application-exit', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-confirmed-daemon-restart', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--codex', '--confirmed', '--restart-daemon', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-menu-compat', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--codex', '--without-hold-api', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-external-tmux', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--external', terminal_executable.full_path()],
    depends: terminal_executable, timeout: 120, is_parallel: false)
  test('terminal-update-wayland-external-tmux-shell', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--external', '--tmux-shell', terminal_executable.full_path()],
    depends: terminal_executable, timeout: 120, is_parallel: false)
  test('terminal-update-wayland-external-shell', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--external', '--shell', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-external-codex', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--external', '--codex', '--restart-daemon', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
  test('terminal-update-wayland-external-without-tab-api', find_program('python3'),
    args: [files('test-terminal-update-wayland.py'), '--external', '--without-tab-api', '--codex', '--confirm-module',
           update_confirm_module.full_path(), terminal_executable.full_path()],
    depends: [terminal_executable, update_confirm_module], timeout: 120, is_parallel: false)
endif
