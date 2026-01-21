project('libant', 'c', default_options: [
  'optimization=2',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=false',
  'strip=true'
], subproject_dir: 'vendor')

use_lto = get_option('lto')
cmake = import('cmake')
src_root = meson.project_source_root() / '..'

module_files = run_command('sh', '-c', 
  'ls ' + src_root / 'src' / 'modules' / '*.c',
  check: true
).stdout().strip().split()

lib_sources = files(
  '../src/utils.c',
  '../src/ant.c',
  '../src/gc.c',
  '../src/repl.c',
  '../src/runtime.c',
  '../src/snapshot.c',
  '../src/esm/remote.c',
) + files(module_files)

tls_lib = get_option('tls_library')
cc = meson.get_compiler('c')

if tls_lib == 'openssl'
  openssl_dep = dependency('openssl', required: true, static: true)
  mbedtls_dep = []
else
  openssl_dep = dependency('', required: false)
  mbedtls_dep = [
    dependency('mbedtls', required: true, static: true),
    dependency('mbedx509', required: true, static: true),
    dependency('mbedcrypto', required: true, static: true),
  ]
endif
libsodium_dep = dependency('libsodium', required: true, static: true)

if host_machine.system() == 'windows'
  uuid_dep = cc.find_library('rpcrt4', required: true)
elif host_machine.system() == 'darwin'
  uuid_dep = dependency('uuid', required: true)
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
else
  uuid_dep = dependency('uuid', required: true, static: true)
endif

llhttp = dependency('llhttp', static: true, required: false)
if not llhttp.found()
  llhttp = dependency('llhttp', method: 'cmake', modules: ['llhttp::llhttp_static'], required: true)
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
  'TLSUV_TLSLIB': tls_lib,
  'MBEDTLS_DIR': cmake_prefix,
}
tlsuv_opts.add_cmake_defines(tlsuv_cmake_defines)

bdwgc_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'enable_cplusplus': 'OFF',
})

tlsuv_compile_args = [
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-bitwise-op-parentheses',
  '-Wno-sometimes-uninitialized',
  '-I' + meson.project_source_root() / 'subprojects' / 'libuv-v1.51.0' / 'include'
]
if cmake_prefix != ''
  tlsuv_compile_args += ['-I' + cmake_prefix / 'include']
endif
tlsuv_opts.append_compile_args('c', tlsuv_compile_args)

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
    '--manifest-path ' + meson.project_source_root() / '..' / 'src' / 'strip' / 'Cargo.toml' + ' ' +
    '--target-dir ' + meson.project_build_root() / 'oxc-target' +
    ' && ' + cp.full_path() + ' ' + oxc_release_dir / oxc_lib_name + ' @OUTPUT@'
  ],
  build_by_default: true
)

oxc_dep = declare_dependency(link_with: oxc_lib)

git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
if git_hash == ''
  git_hash = 'unknown'
endif

timestamp_opt = get_option('build_timestamp')
if timestamp_opt == ''
  timestamp = run_command('date', '+%s', check: true).stdout().strip()
else
  timestamp = timestamp_opt
endif

version_conf = configuration_data()
version_conf.set('ANT_VERSION', '0.3.13.' + timestamp + '-g' + git_hash)
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
  input: '../include/config.h.in',
  output: 'config.h',
  configuration: version_conf
)

include = include_directories('../include')
build_include = include_directories('.')
strip_include = include_directories('../src/strip')

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

core_files = run_command('sh', '-c', 
  'find ' + meson.project_source_root() / '..' / 'src' / 'core' + ' -name "*.js" ! -name "index.js" | sort',
  check: true
).stdout().strip().split()

snapshot_h = custom_target(
  'snapshot',
  input: ['../src/core/index.ts'] + files(core_files),
  output: 'snapshot_data.h',
  command: [
    node,
    meson.project_source_root() / '..' / 'src' / 'tools' / 'gen_snapshot.js',
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
  llhttp, pcre2_dep, libuv_dep,
  argtable3_dep, tlsuv_dep, libsodium_dep,
  yyjson_dep, minicoro_dep, uuidv7_dep,
  openssl_dep, zlib_dep, uthash_dep,
] + mbedtls_dep

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
endif

libant_core = static_library(
  'ant_core',
  lib_sources + [snapshot_h],
  include_directories: [include, build_include, strip_include],
  dependencies: ant_deps + [oxc_dep],
)

if use_lto
  libant_core_lto = static_library(
    'ant_core_lto',
    lib_sources + [snapshot_h],
    include_directories: [include, build_include, strip_include],
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

gen_header = custom_target(
  'ant_header',
  input: [config_h],
  output: 'libant.h',
  command: [
    'bash', meson.project_source_root() / 'scripts' / 'header.sh',
    '@INPUT0@', '@OUTPUT@'
  ],
  build_by_default: true
)