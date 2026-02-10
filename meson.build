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

src_root = meson.project_source_root()
vendor_dir = 'vendor'
is_static = get_option('static_link')

subdir('meson')

module_files = run_command('sh', '-c', 
  'cd "$MESON_SOURCE_ROOT" && ls src/modules/*.c',
  check: true
).stdout().strip().split()

lib_sources = files(
  'src/roots.c',
  'src/utils.c',
  'src/utf8.c',
  'src/strings.c',
  'src/reactor.c',
  'src/sugar.c',
  'src/ant.c',
  'src/errors.c',
  'src/stack.c',
  'src/gc.c',
  'src/repl.c',
  'src/runtime.c',
  'src/snapshot.c',
  'src/esm/remote.c',
  'src/cli/pkg.c',
) + files(module_files)

include = include_directories('include')
build_include = include_directories('.')
strip_include = include_directories('src/strip')

libant = static_library(
  'ant',
  lib_sources + [snapshot_h],
  include_directories: [
    include, version_include, 
    build_include, strip_include
  ],
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
  include_directories: [
    include, version_include, build_include
  ],
  dependencies: ant_deps + [oxc_dep, pkg_dep]
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

executable(
  'ant',
  files('src/main.c') + [snapshot_h],
  include_directories: [strip_include],
  dependencies: libant_dep,
  link_args: link_args
)