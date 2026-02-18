project('ant', 'c', default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
is_static = get_option('static_link')

src_root = meson.project_source_root()
include = include_directories('include')

subdir('meson')

lib_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'engine',
  check: true
).stdout().strip().split())

libant = static_library(
  'ant',
  lib_sources + [snapshot_h],
  include_directories: include,
  dependencies: ant_deps,
  install: true
)

pkg_lib = custom_target(
  'pkg_zig',
  output: 'libpkg.a',
  command: [
    'sh', '-c',
    '"$ZIG" build --build-file "$PKG_ZIG_DIR/build.zig" --prefix "$PKG_BUILD_DIR" && cp "$PKG_BUILD_DIR/lib/$PKG_LIB_NAME" "@OUTPUT@"'
  ],
  env: {
    'ZIG': zig.full_path(),
    'ANT_VERSION': ant_version,
    
    'PKG_ZIG_DIR': pkg_zig_dir, 'PKG_BUILD_DIR': pkg_build_dir,
    'LMDB_INCLUDE': lmdb_include_path, 'ZLIB_INCLUDE': zlib_include_path,
    'LIBUV_INCLUDE': libuv_include_path, 'YYJSON_INCLUDE': yyjson_include_path,
    
    'PKG_TARGET': host_machine.cpu_family() + '-' + host_machine.system(),
    'PKG_LIB_NAME': host_machine.system() == 'windows' ? 'pkg.lib' : 'libpkg.a',
  },
  build_by_default: true,
  build_always_stale: true
)

pkg_dep = declare_dependency(
  link_with: [pkg_lib],
  include_directories: include,
  dependencies: [lmdb_dep, tlsuv_dep, libuv_dep, nghttp2_dep]
)

libant_dep = declare_dependency(
  link_with: libant,
  include_directories: include,
  dependencies: ant_deps + [oxc_dep, pkg_dep]
)

messages_h = custom_target('messages_h',
  input: 'src/core/messages.toml',
  output: 'messages.h',
  command: [python, files('meson/messages.py'), '@INPUT@'],
  capture: true,
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

executable(
  'ant',
  files('src/main.c', 'src/watch.c') + [messages_h],
  dependencies: libant_dep, link_args: link_args
)
