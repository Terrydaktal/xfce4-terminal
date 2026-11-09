project(
  'xfce4-terminal',
  'c',
version : '1.2.0-dev',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 1.3.2',
  default_options : ['c_std=gnu11', 'buildtype=debugoptimized', 'warning_level=2']
)

project_namespace = 'apps'
pkgdatadir = get_option('prefix') / get_option('datadir') / meson.project_name()
copyright_year = '2026'
terminal_version_dbus = '5'

cc = meson.get_compiler('c')
pkgconfig = import('pkgconfig')
gnome = import('gnome')
i18n = import('i18n')
fs = import('fs')

dependency_versions = {
  'glib': '>= 2.66.0',
  'gtk': '>= 3.24.0',
  'vte': '>= 0.51.3',
  'pcre2': '>= 10.00',
  'xfce4': '>= 4.18.0',

  'libx11': '>= 1.6.7',
  'gtk-layer-shell': '>= 0.7.0',
}

glib = dependency('glib-2.0', version: dependency_versions['glib'])
gio = dependency('gio-2.0', version: dependency_versions['glib'])
json_glib = dependency('json-glib-1.0', version: '>= 1.6.0')
gtk = dependency('gtk+-3.0', version: dependency_versions['gtk'])
vte_install_rpath = ''
if get_option('patched-vte')
  # Keep this dependency private to the terminal: never replace the system VTE
  # or export a library search path into shells and applications started here.
  vte_project = subproject('vte', default_options: [
    'gtk3=true',
    'gtk4=false',
    'app=false',
    'gir=false',
    'vapi=false',
    'glade=false',
    'docs=false',
    'tests=false',
  ])
  vte = vte_project.get_variable('libvte_gtk3_dep')
  vte_install_rpath = '$ORIGIN' / fs.relative_to(
    get_option('prefix') / get_option('libdir') / 'xfce4-terminal',
    get_option('prefix') / get_option('bindir'),
  )
else
  vte = dependency('vte-2.91', version: dependency_versions['vte'])
endif
pcre2 = dependency('libpcre2-8', version: dependency_versions['pcre2'])
libxfce4ui = dependency('libxfce4ui-2', version: dependency_versions['xfce4'])
libxfce4kbd = dependency('libxfce4kbd-private-3', version: dependency_versions['xfce4'])
libxfce4util = dependency('libxfce4util-1.0', version: dependency_versions['xfce4'])
xfconf = dependency('libxfconf-0', version: dependency_versions['xfce4'])

# Feature: 'x11'
x11_deps = []
x11_deps += dependency('gdk-x11-3.0', version: dependency_versions['gtk'], required: get_option('x11'))
x11_deps += dependency('x11', version: dependency_versions['libx11'], required: get_option('x11'))

# Feature: 'wayland'
wayland_deps = []
wayland_deps += dependency('gdk-wayland-3.0', version: dependency_versions['gtk'], required: get_option('wayland'))

feature_cflags = []
if get_option('patched-vte')
  # main.c calls a symbol exported only by the patched library. A missing
  # private library must fail at load time instead of silently using system VTE.
  feature_cflags += '-DHAVE_PRIVATE_VTE_SELECTION_FIX=1'
  feature_cflags += '-DHAVE_PRIVATE_VTE_SCROLLBACK_EXPORT=1'
endif

enable_x11 = not get_option('x11').disabled()
foreach dep : x11_deps
  enable_x11 = enable_x11 and dep.found()
endforeach
if enable_x11
  feature_cflags += '-DENABLE_X11=1'
endif

enable_wayland = not get_option('wayland').disabled()
foreach dep : wayland_deps
  enable_wayland = enable_wayland and dep.found()
endforeach
if enable_wayland
  feature_cflags += '-DENABLE_WAYLAND=1'
  gtk_layer_shell = dependency('gtk-layer-shell-0', version: dependency_versions['gtk-layer-shell'], required: get_option('gtk-layer-shell'))
  if gtk_layer_shell.found()
    feature_cflags += '-DHAVE_GTK_LAYER_SHELL=1'
  endif
else
  gtk_layer_shell = dependency('', required: false)
endif

if not enable_x11 and not enable_wayland
  error('At least one of the X11 and Wayland backends must be enabled')
endif

xsltproc = find_program('xsltproc', required: get_option('doc'))

headers = [
  'sys/types.h',
  'errno.h',
  'limits.h',
  'locale.h',
  'memory.h',
  'pwd.h',
  'signal.h',
  'stdarg.h',
  'stdlib.h',
  'string.h',
  'time.h',
  'unistd.h',
]
foreach header : headers
  if cc.check_header(header)
    feature_cflags += '-DHAVE_@0@=1'.format(header.underscorify().to_upper())
  endif
endforeach

libutempter = cc.find_library('utempter', required: get_option('libutempter'))
if libutempter.found()
  feature_cflags += '-DHAVE_LIBUTEMPTER=1'
endif

if host_machine.system() in ['freebsd', 'dragonfly']
  libprocstat = cc.find_library('procstat', required: true)
  libutil = cc.find_library('util', required: true)
else
  libprocstat = dependency('', required: false)
  libutil = dependency('', required: false)
endif

extra_cflags = []
extra_cflags_check = [
  '-Wmissing-declarations',
  '-Wmissing-noreturn',
  '-Wold-style-definition',
  '-Wredundant-decls',
  '-Wpointer-arith',
  '-Wcast-align',
  '-Winit-self',
  '-Wshadow',
  '-Wmissing-include-dirs',
  '-Wundef',
  '-Wformat',
  '-Wformat-security',
  '-Wformat-y2k',
  '-Wnested-externs',
  '-Wno-unused-parameter',
  '-Wno-declaration-after-statement',
  '-Wno-missing-field-initializers',
  '-Werror=implicit-function-declaration',
  '-Wno-error=deprecated-declarations',
]

optimization = get_option('optimization')
if get_option('debug') and optimization in ['0', 'g']
  extra_cflags_check += '-fstack-protector-strong'
  extra_cflags += [
    '-DDEBUG=1',
    '-DDEBUG_TRACE=1',
    '-DG_ENABLE_DEBUG',
  ]
elif optimization in ['3', 's']
  extra_cflags += [
    '-DNDEBUG',
    '-DG_DISABLE_CAST_CHECKS',
    '-DG_DISABLE_ASSERT',
  ]
endif

if dependency_versions.has_key('glib')
  glib_version_parts = dependency_versions['glib'].split(' ')
  glib_min_version_parts = glib_version_parts[1].split('.')
  glib_min_version_define = 'GLIB_VERSION_@0@_@1@'.format(glib_min_version_parts[0], glib_min_version_parts[1])
  extra_cflags += [
    '-DGLIB_VERSION_MIN_REQUIRED=@0@'.format(glib_min_version_define),
    '-DGLIB_VERSION_MAX_ALLOWED=@0@'.format(glib_min_version_define),
    '-DG_LOG_USE_STRUCTURED=1',
  ]
endif

version_parts = meson.project_version().split('-dev')[0].split('.')
version_short = '@0@.@1@'.format(version_parts[0], version_parts[1])

extra_cflags += [
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-DPACKAGE_NAME="@0@"'.format(meson.project_name()),
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DVERSION_SHORT="@0@"'.format(version_short),
  '-DPACKAGE_STRING="@0@ @1@"'.format(meson.project_name(), meson.project_version()),
  '-DPACKAGE_DATADIR="@0@"'.format(pkgdatadir),
  '-DCOPYRIGHT_YEAR="@0@"'.format(copyright_year),
  '-DPACKAGE_LOCALE_DIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DPACKAGE_BUGREPORT="https://gitlab.xfce.org/@0@/@1@/-/issues"'.format(project_namespace, meson.project_name()),
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DPREFIX="@0@"'.format(get_option('prefix')),
  '-DBINDIR="@0@"'.format(get_option('prefix') / get_option('bindir')),
  '-DDATADIR="@0@"'.format(get_option('prefix') / get_option('datadir')),
  '-DINCLUDEDIR="@0@"'.format(get_option('prefix') / get_option('includedir')),
  '-DLIBDIR="@0@"'.format(get_option('prefix') / get_option('libdir')),
  '-DLIBEXECDIR="@0@"'.format(get_option('prefix') / get_option('libexecdir')),
  '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DLOCALSTATEDIR="@0@"'.format(get_option('prefix') / get_option('localstatedir')),
  '-DSBINDIR="@0@"'.format(get_option('prefix') / get_option('sbindir')),
  '-DSYSCONFDIR="@0@"'.format(get_option('prefix') / get_option('sysconfdir')),
  '-DHAVE_XFCE_REVISION_H=1',
  '-DTERMINAL_UPDATE_HELPER_BUILD="@0@"'.format(meson.source_root() / 'terminal/update/terminal-update-helper.py'),
  '-DTERMINAL_UPDATE_HELPER_INSTALLED="@0@"'.format(get_option('prefix') / get_option('libexecdir') / 'xfce4-terminal/update/terminal-update-helper.py'),
  '-DTERMINAL_UPDATE_PYTHON="@0@"'.format(find_program('python3').path()),
]

add_project_arguments(cc.get_supported_arguments(extra_cflags_check), language: 'c')
add_project_arguments(extra_cflags, language: 'c')
add_project_arguments(feature_cflags, language: 'c')

xfce_revision_h = custom_target(
  'fork-build-version',
  output: ['xfce-revision.h', 'BUILD-VERSION', 'BUILD-ID', 'SOURCE-VERSION', 'SOURCE-COMMIT'],
  command: [find_program('bash'), files('scripts/generate-build-header.sh'), '@OUTPUT0@'],
  depend_files: files('xfce-revision.h.in', 'FORK-VERSION', 'FORK-BASE',
                      'scripts/build-version.sh', 'scripts/source-version.sh'),
  build_always_stale: true,
  build_by_default: true,
)

desktop_files = [
  'xfce4-terminal-settings.desktop.in',
  'xfce4-terminal.desktop.in',
]
foreach file : desktop_files
  i18n.merge_file(
    input: file,
    output: fs.stem(file),
    po_dir: 'po',
    type: 'desktop',
    install: true,
    install_dir: get_option('prefix') / get_option('datadir') / 'applications',
  )
endforeach

subdir('colorschemes')
if get_option('doc')
  subdir('doc')
endif
subdir('icons')
subdir('po')
subdir('terminal')
subdir('tests')
