project('ant', ['c', 'cpp'], default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'cpp_std=gnu++23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
fs = import('fs')
is_static = get_option('static_link')

src_root = meson.project_source_root()
include = include_directories('include')
sandbox_backend_include = include_directories(
  'src/sandbox/backends',
  'src/sandbox/backends/shared/include',
)

sandbox_shared_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:shared',
  check: true
).stdout().strip().split())

sandbox_backend_sources = []
darwin_hvf_supported = host_machine.system() == 'darwin' and host_machine.cpu_family() == 'aarch64'
if host_machine.system() == 'darwin'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin',
    check: true
  ).stdout().strip().split())
  if darwin_hvf_supported
    sandbox_backend_sources += files(run_command(
      python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin_hvf',
      check: true
    ).stdout().strip().split())
    sandbox_backend_sources += sandbox_shared_sources
  endif
elif host_machine.system() == 'linux'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:linux',
    check: true
  ).stdout().strip().split())
  sandbox_backend_sources += sandbox_shared_sources
elif host_machine.system() == 'windows'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:windows',
    check: true
  ).stdout().strip().split())
endif

subdir('meson')

messages_h = custom_target('messages_h',
  input: 'src/cli/messages.toml',
  output: 'messages.h',
  command: [python, files('meson/messages.py'), '@INPUT@'],
  capture: true,
)

generic_step_h = custom_target('generic_step_h',
  input: ['src/silver/engine.c', 'src/silver/ops/handlers.h', 'meson/generic_step.ops'],
  output: 'generic_step_cases.h',
  command: [python, files('meson/generic_step.py'), '@INPUT0@', '@INPUT1@', '@INPUT2@'],
  depend_files: files('meson/generic_step.py'),
  capture: true,
)

defgen = generator(python,
  arguments: [src_root / 'meson/defgen.py', '@INPUT@', '@OUTPUT@', '@DEPFILE@'],
  output: '@BASENAME@',
  depfile: '@BASENAME@.d',
)

silver_macros_h = defgen.process('src/silver/ops/macros.h.in', preserve_path_from: src_root / 'src')

theme_h = custom_target('theme_h',
  input: 'src/highlight/theme.toml',
  output: 'theme.h',
  command: [python, files('meson/theme.py'), '@INPUT@'],
  capture: true,
)

unicode_data_h = custom_target('unicode_data_h',
  output: 'unicode_data.h',
  command: [python, files('meson/unicode_tables.py'), utf8proc_srcdir, '@OUTPUT@'],
  depend_files: files('meson/unicode_tables.py'),
)

lib_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'engine',
  check: true
).stdout().strip().split())

section_gc_c_args = host_machine.system() != 'darwin' ? ['-ffunction-sections', '-fdata-sections'] : []

libant = static_library(
  'ant',
  lib_sources + sandbox_backend_sources + [
    builtins_h,
    generic_step_h,
    messages_h,
    silver_macros_h,
    theme_h,
    unicode_data_h,
  ],
  install: true,
  include_directories: [include, sandbox_backend_include],
  c_args: section_gc_c_args,
  dependencies: ant_deps
)

libant_dep = declare_dependency(
  link_with: libant,
  include_directories: [include, sandbox_backend_include],
  dependencies: ant_deps + temporal_link_deps
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

ant_link_args = link_args
if host_machine.system() == 'windows'
  ant_link_args += ['-Wl,--stack,134217728']
elif host_machine.system() == 'darwin'
  ant_link_args += ['-Wl,-stack_size,0x2000000']
else
  ant_link_args += ['-Wl,-z,stack-size=134217728']
endif
if get_option('linker_map')
  ant_linkmap = meson.project_build_root() + '/ant.linkmap'
  if host_machine.system() == 'darwin'
    ant_link_args += ['-Wl,-map,' + ant_linkmap]
  else
    ant_link_args += ['-Wl,-Map=' + ant_linkmap]
  endif
endif

ant_link_depends = []
if host_machine.system() == 'darwin'
  llvm_nm = find_program(get_option('llvm_nm'))
  ant_exports = custom_target('ant_exports',
    input: [libant, libuv_lib],
    output: 'ant.exports',
    command: [python, files('meson/exports.py'), llvm_nm, '@INPUT@'],
    capture: true,
  )
  ant_link_args += [
    '-Wl,-exported_symbols_list,' + ant_exports.full_path(),
    '-Wl,-dead_strip',
  ]
  ant_link_depends += [ant_exports]
endif

if host_machine.system() == 'linux'
  ant_link_args += [
    '-Wl,--gc-sections',
    '-Wl,--dynamic-list=' + meson.current_source_dir() / 'meson/ant.dynlist',
  ]
  ant_link_depends += files('meson/ant.dynlist')
endif

ant_exe = executable(
  'ant',
  files('src/main.c') + [messages_h],
  dependencies: libant_dep,
  link_args: ant_link_args,
  link_depends: ant_link_depends,
  export_dynamic: host_machine.system() == 'windows'
)

if host_machine.system() == 'darwin' and get_option('codesign')
  codesign = find_program('codesign')
  custom_target(
    'ant_codesign',
    input: files('meson/ant.entitlements'),
    output: 'ant.codesign.stamp',
    command: [
      'sh', '-c',
      '"$1" --force --sign - --entitlements "$2" "$3" && touch "$4"',
      'ant-codesign',
      codesign,
      '@INPUT0@',
      ant_exe,
      '@OUTPUT@',
    ],
    depends: ant_exe,
    build_by_default: true,
  )
endif

runtime_binary = get_option('runtime_binary')
if not runtime_binary.disabled()
  runtime_sources = files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'runtime',
    check: true
  ).stdout().strip().split())

  build_runtime_by_default = runtime_binary.enabled()

  runtime_c_args = []
  ant_runtime_link_args = link_args
  if host_machine.system() == 'windows'
    ant_runtime_link_args += ['-Wl,--stack,134217728']
  elif host_machine.system() == 'darwin'
    ant_runtime_link_args += ['-Wl,-stack_size,0x2000000']
  else
    ant_runtime_link_args += ['-Wl,-z,stack-size=134217728']
  endif
  if host_machine.system() != 'darwin'
    runtime_c_args += ['-ffunction-sections', '-fdata-sections']
    ant_runtime_link_args += ['-Wl,--gc-sections']
  endif

  libant_runtime = static_library(
    'ant-runtime-core',
    runtime_sources + [
      builtins_h,
      generic_step_h,
      messages_h,
      silver_macros_h,
      theme_h,
      unicode_data_h,
    ],
    include_directories: [include],
    c_args: runtime_c_args,
    dependencies: ant_slim_deps,
    build_by_default: build_runtime_by_default,
  )

  ant_runtime_link_depends = []
  if host_machine.system() == 'darwin'
    ant_runtime_exports = custom_target('ant_runtime_exports',
      input: [libant_runtime, libuv_lib],
      output: 'ant-runtime.exports',
      command: [python, files('meson/exports.py'), llvm_nm, '@INPUT@'],
      capture: true,
    )
    ant_runtime_link_args += [
      '-Wl,-exported_symbols_list,' + ant_runtime_exports.full_path(),
      '-Wl,-dead_strip',
    ]
    ant_runtime_link_depends += [ant_runtime_exports]
  elif host_machine.system() == 'linux'
    ant_runtime_link_args += [
      '-Wl,--dynamic-list=' + meson.current_source_dir() / 'meson/ant.dynlist',
    ]
    ant_runtime_link_depends += files('meson/ant.dynlist')
  endif

  ant_runtime_exe = executable(
    'ant-runtime',
    files('src/main_runtime.c') + [messages_h],
    dependencies: declare_dependency(
      link_with: libant_runtime,
      include_directories: [include],
      dependencies: ant_slim_deps + temporal_link_deps
    ),
    link_args: ant_runtime_link_args,
    link_depends: ant_runtime_link_depends,
    export_dynamic: false,
    build_by_default: build_runtime_by_default,
  )

  if host_machine.system() == 'darwin' and get_option('codesign')
    custom_target(
      'ant_runtime_codesign',
      output: 'ant-runtime.codesign.stamp',
      command: [
        'sh', '-c',
        '"$1" -x "$2" && "$3" --force --sign - "$2" && touch "$4"',
        'ant-runtime-codesign',
        find_program('strip'),
        ant_runtime_exe,
        codesign,
        '@OUTPUT@',
      ],
      depends: ant_runtime_exe,
      build_by_default: build_runtime_by_default,
    )
  endif
endif

subdir('meson/tests')

embed_example = get_option('embed_example')
embed_example_dist = 'packages/libant/dist'

if not embed_example.disabled()
  if fs.is_dir(meson.project_source_root() / embed_example_dist)
    executable(
      'embed-example',
      files('examples/embed/embed.c'),
      include_directories: include_directories(embed_example_dist),
      dependencies: libant_dep,
      build_by_default: false
    )
  elif embed_example.enabled()
    error('embed_example requires ' + embed_example_dist + '; run packages/libant/build.sh first')
  endif
endif
