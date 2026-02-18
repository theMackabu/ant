project('libant', 'c', default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=false',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
is_static = true

use_lto = get_option('lto')
src_root = meson.project_source_root() / '..'
include = include_directories('../include')

subdir('meson')

messages_h = custom_target('messages_h',
  input: '../src/core/messages.toml',
  output: 'messages.h',
  command: [python, files('../meson/messages.py'), '@INPUT@'],
  capture: true,
)

lib_sources = files(run_command(
  python, files('meson/sources.py'), src_root / 'sources.json', 'library', src_root,
  check: true
).stdout().strip().split())

libant_core = static_library(
  'ant_core',
  lib_sources + [snapshot_h, messages_h],
  include_directories: include,
  dependencies: ant_deps + [oxc_dep],
)

if use_lto
  libant_core_lto = static_library(
    'ant_core_lto',
    lib_sources + [snapshot_h, messages_h],
    include_directories: include,
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
  version: ant_version,
  description: 'javascript for 🐜\'s',
  extra_cflags: ['-I${includedir}/ant'],
  libraries: ['-L${libdir}', '-lant'] + pkg_libs
)

custom_target(
  'ant_header',
  output: 'libant.h',
  command: [
    'bash', meson.project_source_root() / 'scripts' / 'header.sh',
    '@OUTPUT@', ant_version, timestamp, git_hash, target_triple,
  ],
  build_by_default: true,
  build_always_stale: true
)
