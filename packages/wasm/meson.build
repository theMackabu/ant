project('ant-wasm', ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'buildtype=release',
    'default_library=static',
    'c_std=gnu23',
    'cpp_std=gnu++23',
    'b_lto=true',
    'warning_level=2',
  ],
  subproject_dir: 'vendor',
)

root = meson.project_source_root() / '../..'
node = find_program('node')
abi_json = files('abi.json')
abi_value = files('scripts/abi-value.mjs')
initial_memory = run_command(
  node, abi_value, abi_json, 'INITIAL_MEMORY', check: true,
).stdout().strip()
maximum_memory = run_command(
  node, abi_value, abi_json, 'MAX_MEMORY_LIMIT', check: true,
).stdout().strip()
inc = include_directories(
  '.cache/generated',
  '../../include',
)

base64_dep = declare_dependency(
  dependencies: subproject('aklomp-base64').get_variable('base64_dep'),
  compile_args: ['-DBASE64_STATIC_DEFINE'],
)
pcre2_dep = subproject('pcre2', default_options: [
  'grep=false',
  'jit=disabled',
  'test=false',
  'warning_level=0',
]).get_variable('libpcre2_8')
utf8proc_dep = subproject('utf8proc', default_options: [
  'tests=disabled',
]).get_variable('utf8proc_dep')
uthash_dep = subproject('uthash').get_variable('uthash_dep')
crprintf_dep = subproject('crprintf').get_variable('crprintf_dep')
double_conversion_dep = subproject('double-conversion').get_variable('double_conversion_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')

python = find_program('python3')
unicode_tables = files('../../meson/unicode_tables.py')
unicode_data_h = custom_target('unicode_data_h',
  output: 'unicode_data.h',
  command: [python, unicode_tables, meson.current_source_dir() / 'vendor/utf8proc-2.10.0', '@OUTPUT@'],
  depend_files: unicode_tables,
)

defgen = generator(python,
  arguments: [root / 'meson/defgen.py', '@INPUT@', '@OUTPUT@', '@DEPFILE@'],
  output: '@BASENAME@',
  depfile: '@BASENAME@.d',
)

silver_macros_h = defgen.process(root / 'src/silver/ops/macros.h.in', preserve_path_from: root / 'src')

core_sources = files(run_command(
  python, root / 'meson/sources.py', root / 'sources.json', 'wasm', root, '../..',
  check: true,
).stdout().strip().split()) + files(
  'src/bridge.c',
  'src/stubs.c',
  'src/host_stub.c',
  'src/jit_stub.c',
  'src/platform.c',
  'src/utils.c',
)

add_project_arguments(
  '-DANT_WASM_EMBED=1',
  '-DNO_EXECUTE_PERMISSION',
  '-DPCRE2_STATIC',
  '-DUTF8PROC_STATIC',
  '-fno-math-errno',
  '-fno-trapping-math',
  '-ffunction-sections',
  '-fdata-sections',
  language: ['c', 'cpp'],
)

add_project_arguments(
  '-Wno-unused-parameter',
  language: 'c',
)

executable('ant', core_sources, silver_macros_h, unicode_data_h,
  include_directories: inc,
  dependencies: [
    base64_dep,
    pcre2_dep,
    utf8proc_dep,
    uthash_dep,
    crprintf_dep,
    double_conversion_dep,
    yyjson_dep,
  ],
  link_args: [
    '-mexec-model=reactor',
    '-Wl,--import-memory',
    '-Wl,--initial-memory=' + initial_memory,
    '-Wl,--max-memory=' + maximum_memory,
    '-Wl,-z,stack-size=1048576',
    '-Wl,--gc-sections',
    '-Wl,--fatal-warnings',
    '-Wl,--allow-undefined-file=' + meson.current_source_dir() / 'src/imports.txt',
    '-Wl,--error-limit=0',
  ],
  name_suffix: 'wasm',
  install: false,
)
