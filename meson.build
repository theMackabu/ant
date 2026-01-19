project('ant', 'c', default_options: [
  'optimization=2',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=true',
  'strip=true'
], subproject_dir: 'vendor')

cmake = import('cmake')
include = include_directories('include')

module_files = run_command('sh', '-c', 
  'cd "$MESON_SOURCE_ROOT" && ls src/modules/*.c',
  check: true
).stdout().strip().split()

sources = files(
  'src/main.c',
  'src/utils.c',
  'src/ant.c',
  'src/gc.c',
  'src/repl.c',
  'src/runtime.c',
  'src/snapshot.c',
  'src/esm/remote.c',
) + files(module_files)

tls_lib = get_option('tls_library')
cc = meson.get_compiler('c')
if tls_lib == 'openssl'
  openssl_dep = dependency('openssl', required: true)
  mbedtls_dep = []
else
  openssl_dep = dependency('', required: false)
  mbedtls_dep = [
    dependency('mbedtls', required: true),
    dependency('mbedx509', required: true),
    dependency('mbedcrypto', required: true),
  ]
endif
libsodium_dep = dependency('libsodium', required: true)

if host_machine.system() == 'windows'
  uuid_dep = cc.find_library('rpcrt4', required: true)
  secur32_dep = cc.find_library('secur32', required: true)
  ntdll_dep = cc.find_library('ntdll', required: true)
  crypt32_dep = cc.find_library('crypt32', required: true)
  userenv_dep = cc.find_library('userenv', required: true)
else
  uuid_dep = dependency('uuid', required: true)
endif
llhttp = dependency('llhttp', static: true, required: false)
if not llhttp.found()
  llhttp = dependency('llhttp', method: 'cmake', modules: ['llhttp::llhttp_static'], required: true)
endif

if host_machine.system() == 'darwin'
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
endif

libuv_sub = subproject('libuv', default_options: [
  'build_tests=false', 
  'build_benchmarks=false'
])

libuv_dep = libuv_sub.get_variable('libuv_dep')
tlsuv_opts = cmake.subproject_options()
bdwgc_opts = cmake.subproject_options()

cmake_prefix = get_option('deps_prefix_cmake')
tlsuv_cmake_defines = {
  'BUILD_TESTING': 'OFF',
  'TLSUV_TLSLIB': tls_lib
}
if cmake_prefix != ''
  tlsuv_cmake_defines += {'CMAKE_PREFIX_PATH': cmake_prefix}
endif
tlsuv_opts.add_cmake_defines(tlsuv_cmake_defines)

bdwgc_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'enable_cplusplus': 'OFF',
})

tlsuv_opts.append_compile_args('c', [
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-bitwise-op-parentheses',
  '-Wno-sometimes-uninitialized',
  '-I' + (meson.project_source_root() / 'vendor' / 'libuv-v1.51.0' / 'include')
])

bdwgc_gc_args = ['-DGC_NO_THREAD_REDIRECTS', '-DGC_THREADS']
if host_machine.system() == 'linux'
  bdwgc_gc_args += ['-DNO_GETCONTEXT']
endif
bdwgc_opts.append_compile_args('c', bdwgc_gc_args)

tlsuv_dep = cmake.subproject('tlsuv', options: tlsuv_opts).dependency('tlsuv')
bdwgc_dep = cmake.subproject('bdwgc', options: bdwgc_opts).dependency('gc')

pcre2_dep = subproject('pcre2').get_variable('libpcre2_8')
uthash_dep = subproject('uthash').get_variable('uthash_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')
argtable3_dep = subproject('argtable3').get_variable('argtable3_dep')
minicoro_dep = subproject('minicoro').get_variable('minicoro_dep')

zlib_dep = subproject('zlib-ng',
  default_options: ['b_lto=false']
).get_variable('zlib_ng_dep')

libffi_dep = subproject('libffi', default_options: [
  'warning_level=0',
  'tests=false'
]).get_variable('ffi_dep')

cargo = find_program('cargo', required: true)
cp = find_program('cp', required: true)
strip_include = include_directories('src/strip')

oxc_lib_name = 'liboxc.a'
oxc_output_name = 'liboxc.a'

if host_machine.system() == 'windows'
  rust_target = 'x86_64-pc-windows-gnu'
  oxc_release_dir = meson.project_build_root() / 'oxc-target' / rust_target / 'release'
  rust_target_arg = ' --target ' + rust_target
else
  oxc_release_dir = meson.project_build_root() / 'oxc-target' / 'release'
  rust_target_arg = ''
endif

oxc_lib = custom_target(
  'oxc_strip',
  output: oxc_output_name,
  command: [
    'sh', '-c',
    cargo.full_path() + ' build --release' + rust_target_arg + ' ' +
    '--manifest-path ' + meson.project_source_root() / 'src' / 'strip' / 'Cargo.toml' + ' ' +
    '--target-dir ' + meson.project_build_root() / 'oxc-target' +
    ' && ' + cp.full_path() + ' ' + oxc_release_dir / oxc_lib_name + ' @OUTPUT@'
  ],
  build_by_default: true
)

oxc_dep = declare_dependency(link_with: oxc_lib)

git = find_program('git', required: false)
if git.found()
  git_commit = run_command(git, 'rev-parse', '--short=10', 'HEAD', check: false)
  if git_commit.returncode() == 0
    git_hash = git_commit.stdout().strip()
  else
    git_hash = 'unknown'
  endif
else
  git_hash = 'unknown'
endif

version_conf = configuration_data()

git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
timestamp_opt = get_option('build_timestamp')
if timestamp_opt == ''
  timestamp = run_command('date', '+%s', check: true).stdout().strip()
else
  timestamp = timestamp_opt
endif

version_conf.set('ANT_VERSION', '0.3.11.' + timestamp + '-g' + git_hash)
version_conf.set('ANT_BUILD_TIMESTAMP', timestamp)

cmd_cc = meson.get_compiler('c')
target_triple = run_command(cmd_cc.cmd_array(), '-dumpmachine', check: true).stdout().strip()

triple_parts = target_triple.split('-')
if triple_parts.length() == 4 and triple_parts[1] != 'unknown' and triple_parts[1] != 'pc' and triple_parts[1] != 'apple'
  target_triple = triple_parts[0] + '-unknown-' + triple_parts[2] + '-' + triple_parts[3]
endif

version_conf.set('ANT_GIT_HASH', git_hash)
if tls_lib == 'mbedtls'
  version_conf.set('ANT_TARGET_TRIPLE', target_triple + '-mbedtls')
else
  version_conf.set('ANT_TARGET_TRIPLE', target_triple)
endif

config_h = configure_file(
  input: 'include/config.h.in',
  output: 'config.h',
  configuration: version_conf
)

build_include = include_directories('.')

add_project_arguments(
  '-D NO_EXECUTE_PERMISSION',
  '-Wno-unused-function',
  '-Wno-deprecated-declarations',
language: 'c')

if host_machine.system() == 'linux'
  add_project_arguments('-fno-pie', language: 'c')
  add_project_link_arguments('-no-pie', language: 'c')
endif

node = find_program('node', required: true)
gen_snapshot = files('src/tools/gen_snapshot.js')

core_files = run_command('sh', '-c', 
  'cd "$MESON_SOURCE_ROOT" && find src/core -name "*.js" ! -name "index.js" | sort',
  check: true
).stdout().strip().split()

snapshot_h = custom_target(
  'snapshot',
  input: ['src/core/index.ts'] + files(core_files),
  output: 'snapshot_data.h',
  command: [
    node,
    gen_snapshot,
    '@INPUT0@', 
    '@OUTPUT@',
    'VERSION=' + version_conf.get('ANT_VERSION'),
    'GIT_HASH=' + version_conf.get('ANT_GIT_HASH'),
    'BUILD_TIMESTAMP=' + version_conf.get('ANT_BUILD_TIMESTAMP'),
    'TARGET=' + version_conf.get('ANT_TARGET_TRIPLE'),
    'MBEDTLS=' + (tls_lib == 'mbedtls').to_string(),
    'HOST=' + host_machine.system()
  ],
  build_by_default: true
)

ant_deps = [
  libffi_dep, bdwgc_dep, uuid_dep,
  llhttp, pcre2_dep, libuv_dep, oxc_dep,
  argtable3_dep, tlsuv_dep, libsodium_dep,
  yyjson_dep, minicoro_dep, uuidv7_dep,
  openssl_dep, zlib_dep, uthash_dep,
] + mbedtls_dep

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
elif host_machine.system() == 'windows'
  ant_deps += [secur32_dep, ntdll_dep, crypt32_dep, userenv_dep]
endif

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

executable(
  'ant',
  sources + [snapshot_h],
  include_directories: [include, build_include, strip_include],
  dependencies: ant_deps,
  link_args: link_args
)
