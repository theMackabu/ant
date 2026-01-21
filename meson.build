project('ant', 'c', default_options: [
  'buildtype=release',
  'optimization=2',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true'
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
  'src/utils.c',
  'src/ant.c',
  'src/gc.c',
  'src/repl.c',
  'src/runtime.c',
  'src/snapshot.c',
  'src/esm/remote.c',
) + files(module_files)

include = include_directories('include')
build_include = include_directories('.')
strip_include = include_directories('src/strip')

add_project_arguments(
  '-D NO_EXECUTE_PERMISSION',
  '-Wno-unused-function',
  '-Wno-deprecated-declarations',
language: 'c')

if host_machine.system() == 'linux'
  add_project_arguments('-fno-pie', language: 'c')
  add_project_link_arguments('-no-pie', language: 'c')
endif

libant = static_library(
  'ant',
  lib_sources + [snapshot_h],
  include_directories: [include, build_include, strip_include],
  dependencies: ant_deps,
  install: true
)

libant_dep = declare_dependency(
  link_with: libant,
  include_directories: [include, build_include],
  dependencies: ant_deps + [oxc_dep]
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

executable(
  'ant',
  files('src/main.c') + [snapshot_h],
  include_directories: [include, build_include, strip_include],
  link_with: libant,
  dependencies: ant_deps + [oxc_dep],
  link_args: link_args
)

if get_option('build_examples')
  executable(
    'embed_example',
    files('examples/embed/embed.c'),
    include_directories: [include, build_include],
    link_with: libant,
    dependencies: ant_deps + [oxc_dep]
  )
endif
