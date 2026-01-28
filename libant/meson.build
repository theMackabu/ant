project('libant', 'c', default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=false',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

use_lto = get_option('lto')

src_root = meson.project_source_root() / '..'
vendor_dir = 'vendor'
is_static = true

subdir('meson')

module_files = run_command('sh', '-c', 
  'ls ' + src_root / 'src' / 'modules' / '*.c',
  check: true
).stdout().strip().split()

lib_sources = files(
  '../src/roots.c',
  '../src/utils.c',
  '../src/utf8.c',
  '../src/reactor.c',
  '../src/sugar.c',
  '../src/ant.c',
  '../src/errors.c',
  '../src/stack.c',
  '../src/gc.c',
  '../src/repl.c',
  '../src/runtime.c',
  '../src/snapshot.c',
  '../src/esm/remote.c',
) + files(module_files)

include = include_directories('../include')
build_include = include_directories('.')
strip_include = include_directories('../src/strip')

libant_core = static_library(
  'ant_core',
  lib_sources + [snapshot_h],
  include_directories: [
    include,
    build_include,
    version_include,
    strip_include
  ],
  dependencies: ant_deps + [oxc_dep],
)

if use_lto
  libant_core_lto = static_library(
    'ant_core_lto',
    lib_sources + [snapshot_h],
    include_directories: [
      include,
      build_include,
      version_include,
      strip_include
    ],
    dependencies: ant_deps + [oxc_dep],
    c_args: ['-flto'],
  )
endif

pkg = import('pkgconfig')

if host_machine.system() == 'darwin'
  pkg_libs = ['-framework Security', '-framework CoreFoundation', '-lpthread']
elif host_machine.system() == 'linux'
  pkg_libs = ['-lpthread', '-ldl', '-lm']
elif host_machine.system() == 'windows'
  pkg_libs = ['-lws2_32', '-lrpcrt4', '-lsecur32', '-lntdll', '-lcrypt32', '-luserenv']
else
  pkg_libs = ['-lpthread']
endif

pkg.generate(
  name: 'libant',
  description: 'Ant JavaScript Engine - Embeddable JS runtime',
  version: version_conf.get('ANT_VERSION'),
  extra_cflags: ['-I${includedir}/ant'],
  libraries: ['-L${libdir}', '-lant'] + pkg_libs
)

custom_target(
  'ant_header',
  input: [config_h],
  output: 'libant.h',
  command: [
    'bash', meson.project_source_root() / 'scripts' / 'header.sh',
    '@INPUT0@', '@OUTPUT@'
  ],
  build_by_default: true,
  build_always_stale: true
)
