cmake = import('cmake')
cc = meson.get_compiler('c')
tls_lib = get_option('tls_library')

if tls_lib == 'openssl'
  openssl_dep = dependency('openssl', required: true, static: is_static)
  mbedtls_dep = []
else
  openssl_dep = dependency('', required: false)
  mbedtls_dep = [
    dependency('mbedtls', required: true, static: is_static),
    dependency('mbedx509', required: true, static: is_static),
    dependency('mbedcrypto', required: true, static: is_static),
  ]
endif

libsodium_dep = dependency('libsodium', required: true, static: is_static)

if host_machine.system() == 'windows'
  uuid_dep = cc.find_library('rpcrt4', required: true)
  secur32_dep = cc.find_library('secur32', required: true)
  ntdll_dep = cc.find_library('ntdll', required: true)
  crypt32_dep = cc.find_library('crypt32', required: true)
  userenv_dep = cc.find_library('userenv', required: true)
  win_deps = [secur32_dep, ntdll_dep, crypt32_dep, userenv_dep]
elif host_machine.system() == 'darwin'
  uuid_dep = dependency('uuid', required: true)
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
  win_deps = []
else
  uuid_dep = dependency('uuid', required: true, static: is_static)
  win_deps = []
endif

llhttp = dependency('llhttp', static: true, required: false)
if not llhttp.found()
  llhttp = dependency('llhttp', method: 'cmake', modules: ['llhttp::llhttp_static'], required: true)
endif

libuv_sub = subproject('libuv', default_options: [
  'build_tests=false', 
  'build_benchmarks=false',
  'warning_level=0'
])
libuv_dep = libuv_sub.get_variable('libuv_dep')

tlsuv_opts = cmake.subproject_options()
bdwgc_opts = cmake.subproject_options()

cmake_prefix = get_option('deps_prefix_cmake')
tlsuv_cmake_defines = {
  'BUILD_TESTING': 'OFF',
  'TLSUV_TLSLIB': tls_lib,
}
if cmake_prefix != ''
  tlsuv_cmake_defines += {'MBEDTLS_DIR': cmake_prefix, 'CMAKE_PREFIX_PATH': cmake_prefix}
endif
tlsuv_opts.add_cmake_defines(tlsuv_cmake_defines)

bdwgc_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'enable_cplusplus': 'OFF',
})

tlsuv_compile_args = [
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-parameter',
  '-Wno-sign-compare',
  '-Wno-bitwise-op-parentheses',
  '-Wno-sometimes-uninitialized',
  '-Wno-cast-function-type-mismatch',
  '-Wno-missing-field-initializers',
  '-I' + meson.project_source_root() / vendor_dir / 'libuv-v1.51.0' / 'include'
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

pcre2_dep = subproject('pcre2', default_options: ['warning_level=0']).get_variable('libpcre2_8')
uthash_dep = subproject('uthash').get_variable('uthash_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')
argtable3_dep = subproject('argtable3').get_variable('argtable3_dep')
minicoro_dep = subproject('minicoro').get_variable('minicoro_dep')

zlib_dep = subproject('zlib-ng',
  default_options: ['b_lto=false', 'warning_level=0']
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
    '--manifest-path ' + src_root / 'src' / 'strip' / 'Cargo.toml' + ' ' +
    '--target-dir ' + meson.project_build_root() / 'oxc-target' +
    ' && ' + cp.full_path() + ' ' + oxc_release_dir / oxc_lib_name + ' @OUTPUT@'
  ],
  build_by_default: true
)

oxc_dep = declare_dependency(link_with: oxc_lib)

ant_deps = [
  libffi_dep, bdwgc_dep, uuid_dep,
  llhttp, pcre2_dep, libuv_dep,
  argtable3_dep, tlsuv_dep, libsodium_dep,
  yyjson_dep, minicoro_dep, uuidv7_dep,
  openssl_dep, zlib_dep, uthash_dep,
] + mbedtls_dep + win_deps

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
endif
