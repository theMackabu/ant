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
  'src/uv',
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
dragonbox_dep = subproject('dragonbox').get_variable('dragonbox_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
zlib_dep = subproject('zlib-ng', default_options: [
  'b_lto=false',
  'warning_level=0',
  'zlib-compat=true',
]).get_variable('zlib_ng_dep')
brotli_sub = subproject('google-brotli', default_options: [
  'default_library=static',
  'b_lto=false',
  'warning_level=0',
  'optimization=s',
])
brotli_deps = [
  brotli_sub.get_variable('brotli_common_dep'),
  brotli_sub.get_variable('brotli_decoder_dep'),
  brotli_sub.get_variable('brotli_encoder_dep'),
]
ada_dep = subproject('ada').get_variable('ada_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')

cmake = import('cmake')
boringssl_opts = cmake.subproject_options()
boringssl_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'BUILD_TESTING': 'OFF',
  'INSTALL_ENABLED': '0',
  'OPENSSL_NO_ASM': 'ON',
  'CMAKE_SYSTEM_PROCESSOR': 'wasm32',
})
foreach lang : ['c', 'cpp']
  boringssl_opts.append_compile_args(lang, '-w', '-g0', '-DOPENSSL_SMALL', '-DOPENSSL_NO_ASM', '-DOPENSSL_NO_SOCK', '-D_WASI_EMULATED_GETPID',
    '-DOPENSSL_NO_THREADS_CORRUPT_MEMORY_AND_LEAK_SECRETS_IF_THREADED')
endforeach
boringssl_opts.set_override_option('warning_level', '0')
crypto_dep = cmake.subproject('boringssl', options: boringssl_opts).dependency('crypto')

python = find_program('python3')

fs = import('fs')
ant_version_parts = {}
foreach line : fs.read(root / 'meson/ant.version').strip().split('\n')
  pair = line.strip().split('=')
  ant_version_parts += {pair[0].strip(): pair[1].strip()}
endforeach
ant_git_hash = run_command('git', '-C', root, 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
ant_version = ant_version_parts['major'] + '.' + ant_version_parts['minor'] + '.' + (ant_git_hash == '' ? 'unknown' : ant_git_hash) + '.' + ant_version_parts['patch']

temporal_capi_project = subproject('temporal_capi')
temporal_capi_source = temporal_capi_project.get_variable('temporal_capi_source_dir')
rust_lib = custom_target('rust_staticlib',
  input: files(
    root / 'src/rust/Cargo.toml',
    root / 'src/rust/Cargo.lock',
    root / 'src/rust/src/lib.rs',
    root / 'src/rust/rust-toolchain.toml',
  ) + temporal_capi_project.get_variable('temporal_capi_build_inputs'),
  output: 'libant_rust.a',
  build_by_default: true,
  command: [
    python, files(root / 'meson/rust.py'),
    '--crate', root / 'src/rust',
    '--temporal-capi', temporal_capi_source,
    '--cache', root / '.cache/rust',
    '--target', 'wasm32-wasip1',
    '--out', '@OUTPUT@',
    '--features', 'tzdb-embedded,encoding',
  ],
)
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
  '-D_WASI_EMULATED_GETPID',
  '-DANT_VERSION="' + ant_version + '"',
  '-DANT_HAVE_TEMPORAL=1',
  '-DANT_TEMPORAL_EMBEDDED_TZDB=1',
  '-DANT_HAVE_ENCODING_RS=1',
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
  c_args: ['-I' + temporal_capi_source / 'bindings/c'],
  link_with: rust_lib,
  dependencies: [
    base64_dep,
    pcre2_dep,
    utf8proc_dep,
    uthash_dep,
    crprintf_dep,
    double_conversion_dep,
    dragonbox_dep,
    yyjson_dep,
    zlib_dep,
    brotli_deps,
    ada_dep,
    uuidv7_dep,
    crypto_dep,
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
    '-lwasi-emulated-getpid',
  ],
  name_suffix: 'wasm',
  install: false,
)
