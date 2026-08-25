project('libant', ['c', 'cpp'], default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'cpp_std=gnu++23',
  'default_library=static',
  'b_lto=false',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
fs = import('fs')
is_static = true

use_lto = get_option('lto')
src_root = meson.project_source_root() / '../..'
include = include_directories('../../include')
sandbox_backend_include = include_directories(
  '../../src/sandbox/backends',
  '../../src/sandbox/backends/shared/include',
)

sandbox_shared_sources = files(run_command(
  python, files('meson/sources.py'), src_root / 'sources.json',
  'sandbox_backend:shared', src_root,
  check: true
).stdout().strip().split())

sandbox_backend_sources = []
darwin_hvf_supported = host_machine.system() == 'darwin' and host_machine.cpu_family() == 'aarch64'
if host_machine.system() == 'darwin'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), src_root / 'sources.json',
    'sandbox_backend:darwin', src_root,
    check: true
  ).stdout().strip().split())
  if darwin_hvf_supported
    sandbox_backend_sources += files(run_command(
      python, files('meson/sources.py'), src_root / 'sources.json',
      'sandbox_backend:darwin_hvf', src_root,
      check: true
    ).stdout().strip().split())
    sandbox_backend_sources += sandbox_shared_sources
  endif
elif host_machine.system() == 'linux'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), src_root / 'sources.json',
    'sandbox_backend:linux', src_root,
    check: true
  ).stdout().strip().split())
  sandbox_backend_sources += sandbox_shared_sources
elif host_machine.system() == 'windows'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), src_root / 'sources.json',
    'sandbox_backend:windows', src_root,
    check: true
  ).stdout().strip().split())
endif

subdir('meson')

messages_h = custom_target('messages_h',
  input: '../../src/cli/messages.toml',
  output: 'messages.h',
  command: [python, files('../../meson/messages.py'), '@INPUT@'],
  capture: true,
)

theme_h = custom_target('theme_h',
  input: '../../src/highlight/theme.toml',
  output: 'theme.h',
  command: [python, files('../../meson/theme.py'), '@INPUT@'],
  capture: true,
)

lib_sources = files(run_command(
  python, files('meson/sources.py'), src_root / 'sources.json', 'library', src_root,
  check: true
).stdout().strip().split())

libant_core = static_library(
  'ant_core',
  lib_sources + sandbox_backend_sources + [builtins_h, messages_h, theme_h],
  include_directories: [include, sandbox_backend_include],
  dependencies: ant_deps,
)

if use_lto
  libant_core_lto = static_library(
    'ant_core_lto',
    lib_sources + sandbox_backend_sources + [builtins_h, messages_h, theme_h],
    include_directories: [include, sandbox_backend_include],
    dependencies: ant_deps,
    override_options: ['b_lto=true'],
  )
endif

pkg = import('pkgconfig')

if host_machine.system() == 'darwin'
  pkg_libs = ['-framework Security', '-framework CoreFoundation', '-framework Hypervisor', '-lpthread']
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
    '@OUTPUT@', ant_version, timestamp, git_hash, git_longhash, target_triple,
  ],
  build_by_default: true,
  build_always_stale: true
)
