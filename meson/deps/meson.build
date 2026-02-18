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
  ws2_32_dep = cc.find_library('ws2_32', required: true)
  iphlpapi_dep = cc.find_library('iphlpapi', required: true)
  win_deps = [secur32_dep, ntdll_dep, crypt32_dep, userenv_dep, ws2_32_dep, iphlpapi_dep]
elif host_machine.system() == 'darwin'
  uuid_dep = dependency('uuid', required: true)
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
  win_deps = []
else
  uuid_dep = dependency('uuid', required: true, static: is_static)
  win_deps = []
endif

deps_info = {
  'lmdb': {
    'dir': 'openldap-LMDB_0.9.33',
    'inc': 'libraries/liblmdb',
  },
  'yyjson': {
    'dir': 'yyjson-0.12.0',
    'inc': 'src',
  },
  'libuv': {
    'dir': 'libuv-v1.52.0',
    'inc': 'include',
  },
  'llhttp': {
    'dir': 'llhttp-release-v9.3.1',
    'inc': 'include',
  },
  'zlib-ng': {
    'dir': 'zlib-ng-2.3.3',
    'inc': '',
    'build_dir': true,
  },
}

subprojects_src = src_root / 'vendor'
subprojects_build = meson.project_build_root() / 'vendor'

llhttp = subproject('llhttp').get_variable('llhttp_dep')
libuv_dep = subproject('libuv').get_variable('libuv_dep')

tlsuv_opts = cmake.subproject_options()
tlsuv_opts.set_override_option('warning_level', '0')
tlsuv_opts.append_compile_args('c', ['-w'])

cmake_prefix = get_option('deps_prefix_cmake')
tlsuv_cmake_defines = {
  'BUILD_TESTING': 'OFF',
  'TLSUV_TLSLIB': tls_lib,
  'CMAKE_WARN_DEV': 'OFF',
  'CMAKE_POLICY_DEFAULT_CMP0200': 'NEW',
  'CMAKE_POLICY_DEFAULT_CMP0156': 'NEW',
  'CMAKE_POLICY_DEFAULT_CMP0179': 'NEW',
  'llhttp_DIR': subprojects_src / deps_info['llhttp']['dir'] / 'cmake',
  'libuv_DIR': subprojects_src / deps_info['libuv']['dir'] / 'cmake',
}
if cmake_prefix != ''
  tlsuv_cmake_defines += {'MBEDTLS_DIR': cmake_prefix, 'CMAKE_PREFIX_PATH': cmake_prefix}
endif
tlsuv_opts.add_cmake_defines(tlsuv_cmake_defines)

tlsuv_compile_args = [
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-parameter',
  '-Wno-sign-compare',
  '-Wno-bitwise-op-parentheses',
  '-Wno-sometimes-uninitialized',
  '-Wno-cast-function-type-mismatch',
  '-Wno-missing-field-initializers',
  '-I' + subprojects_src / deps_info['libuv']['dir'] / deps_info['libuv']['inc'],
  '-I' + subprojects_src / deps_info['llhttp']['dir'] / deps_info['llhttp']['inc'],
]

if tls_lib == 'openssl'
  tlsuv_compile_args += ['-I' + openssl_dep.get_variable(pkgconfig: 'includedir')]
endif

if cmake_prefix != ''
  tlsuv_compile_args += ['-I' + cmake_prefix / 'include']
endif

tlsuv_opts.append_compile_args('c', tlsuv_compile_args)
tlsuv_dep = cmake.subproject('tlsuv', options: tlsuv_opts).dependency('tlsuv')

uthash_dep = subproject('uthash').get_variable('uthash_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')
base64_dep = subproject('aklomp-base64').get_variable('base64_dep')
argtable3_dep = subproject('argtable3').get_variable('argtable3_dep')
minicoro_dep = subproject('minicoro').get_variable('minicoro_dep')
crprintf_dep = subproject('crprintf').get_variable('crprintf_dep')

lmdb_dep = subproject('lmdb', default_options: [
  'warning_level=0',
  'programs=disabled'
]).get_variable('lmdb_dep')

zlib_dep = subproject('zlib-ng', default_options: [
  'b_lto=false',
  'warning_level=0'
]).get_variable('zlib_ng_dep')

nghttp2_dep = subproject('nghttp2', default_options: [
  'warning_level=0'
]).get_variable('libnghttp2_dep')

libffi_dep = subproject('libffi', default_options: [
  'warning_level=0',
  'tests=false'
]).get_variable('ffi_dep')

pcre2_dep = subproject('pcre2', default_options: [
  'warning_level=0',
  'grep=false',
  'test=false'
]).get_variable('libpcre2_8')

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
    '"' + cargo.full_path() + '"' + ' build --release' + rust_target_arg + ' ' +
    '--manifest-path "' + src_root / 'src' / 'strip' / 'Cargo.toml' + '" ' +
    '--target-dir "' + meson.project_build_root() / 'oxc-target' + '"' +
    ' && "' + cp.full_path() + '" "' + oxc_release_dir / oxc_lib_name + '" @OUTPUT@'
  ],
  build_by_default: true
)

oxc_dep = declare_dependency(link_with: oxc_lib)
zig = find_program('zig', required: true)

deps_info = {
  'lmdb': {
    'dir': 'openldap-LMDB_0.9.33',
    'inc': 'libraries/liblmdb',
  },
  'yyjson': {
    'dir': 'yyjson-0.12.0',
    'inc': 'src',
  },
  'libuv': {
    'dir': 'libuv-v1.52.0',
    'inc': 'include',
  },
  'llhttp': {
    'dir': 'llhttp-release-v9.3.1',
    'inc': 'include',
  },
  'zlib-ng': {
    'dir': 'zlib-ng-2.3.3',
    'inc': '',
    'build_dir': true,
  },
}

subprojects_src = src_root / 'vendor'
subprojects_build = meson.project_build_root() / 'vendor'

lmdb_include_path = subprojects_src / deps_info['lmdb']['dir'] / deps_info['lmdb']['inc']
yyjson_include_path = subprojects_src / deps_info['yyjson']['dir'] / deps_info['yyjson']['inc']
libuv_include_path = subprojects_src / deps_info['libuv']['dir'] / deps_info['libuv']['inc']
llhttp_include_path = subprojects_src / deps_info['llhttp']['dir'] / deps_info['llhttp']['inc']
zlib_include_path = subprojects_build / deps_info['zlib-ng']['dir'] / deps_info['zlib-ng']['inc']

pkg_zig_dir = src_root / 'src' / 'pkg'
pkg_build_dir = meson.current_build_dir()

ant_deps = [
  libffi_dep, uuid_dep, crprintf_dep,
  llhttp, pcre2_dep, libuv_dep, base64_dep,
  argtable3_dep, tlsuv_dep, libsodium_dep,
  yyjson_dep, minicoro_dep, uuidv7_dep,
  openssl_dep, zlib_dep, uthash_dep, lmdb_dep,
] + mbedtls_dep + win_deps

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
endif
