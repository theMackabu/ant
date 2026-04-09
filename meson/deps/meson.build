cc = meson.get_compiler('c')
cmake = import('cmake')

boringssl_opts = cmake.subproject_options()
boringssl_opts.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'BUILD_TESTING': 'OFF',
  'INSTALL_ENABLED': '0',
})

if host_machine.system() == 'darwin'
  deployment_flag = '-mmacosx-version-min=15.0'
  boringssl_opts.add_cmake_defines({
    'CMAKE_EXE_LINKER_FLAGS': deployment_flag,
    'CMAKE_SHARED_LINKER_FLAGS': deployment_flag,
    'CMAKE_MODULE_LINKER_FLAGS': deployment_flag,
  })
endif

boringssl_warning_flag = cc.get_argument_syntax() == 'msvc' ? '/W0' : '-w'
boringssl_opts.append_compile_args('c', boringssl_warning_flag)
boringssl_opts.append_compile_args('cpp', boringssl_warning_flag)
if host_machine.system() == 'windows'
  boringssl_opts.append_compile_args('c', '-DOPENSSL_WINDOWS')
  boringssl_opts.append_compile_args('cpp', '-DOPENSSL_WINDOWS')
endif
boringssl_opts.set_override_option('warning_level', '0')
boringssl_opts.set_override_option('optimization', 's')

boringssl_sub = cmake.subproject('boringssl', options: boringssl_opts)
ssl_dep = boringssl_sub.dependency('ssl')
crypto_dep = boringssl_sub.dependency('crypto')

meson.override_dependency('ssl', ssl_dep)
meson.override_dependency('crypto', crypto_dep)

if host_machine.system() == 'windows'
  secur32_dep = cc.find_library('secur32', required: true)
  ntdll_dep = cc.find_library('ntdll', required: true)
  crypt32_dep = cc.find_library('crypt32', required: true)
  userenv_dep = cc.find_library('userenv', required: true)
  ws2_32_dep = cc.find_library('ws2_32', required: true)
  iphlpapi_dep = cc.find_library('iphlpapi', required: true)
  pathcch_dep = cc.find_library('pathcch', required: true)
  win_deps = [secur32_dep, ntdll_dep, crypt32_dep, userenv_dep, ws2_32_dep, iphlpapi_dep, pathcch_dep]
elif host_machine.system() == 'darwin'
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'], required: true)
  win_deps = []
else
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
  'google-brotli': {
    'dir': 'brotli-1.1.0',
    'inc': 'c/include',
  },
}

subprojects_src = src_root / 'vendor'
subprojects_build = meson.project_build_root() / 'vendor'

llhttp = subproject('llhttp').get_variable('llhttp_dep')
libuv_dep = subproject('libuv').get_variable('libuv_dep')

zlib_ng_sub = subproject('zlib-ng', default_options: [
  'b_lto=false',
  'warning_level=0',
  'zlib-compat=true'
])
zlib_dep = zlib_ng_sub.get_variable('zlib_ng_dep')
zlib_ng_srcdir = subprojects_src / deps_info['zlib-ng']['dir']
zlib_ng_builddir = subprojects_build / deps_info['zlib-ng']['dir']

brotli_sub = subproject('google-brotli', default_options: [
  'default_library=static',
  'b_lto=false',
  'warning_level=0',
])

brotli_common_dep = brotli_sub.get_variable('brotli_common_dep')
brotli_dec_dep = brotli_sub.get_variable('brotli_decoder_dep')
brotli_enc_dep = brotli_sub.get_variable('brotli_encoder_dep')

cmake_prefix = get_option('deps_prefix_cmake')

tlsuv_sub = subproject('tlsuv', default_options: [
  'tls_backend=boringssl',
  'deps_prefix_cmake=' + cmake_prefix,
  'warning_level=0',
])
tlsuv_dep = tlsuv_sub.get_variable('tlsuv_dep')

base64_dep = declare_dependency(
  dependencies: subproject('aklomp-base64').get_variable('base64_dep'),
  compile_args: ['-DBASE64_STATIC_DEFINE'],
)

uthash_dep = subproject('uthash').get_variable('uthash_dep')
yyjson_dep = subproject('yyjson').get_variable('yyjson_dep')
uuidv7_dep = subproject('uuidv7').get_variable('uuidv7_dep')
argtable3_dep = subproject('argtable3').get_variable('argtable3_dep')
minicoro_dep = subproject('minicoro').get_variable('minicoro_dep')
crprintf_dep = subproject('crprintf').get_variable('crprintf_dep')
uriparser_dep = subproject('uriparser').get_variable('uriparser_dep')
wamr_dep = subproject('wasm-micro-runtime').get_variable('wamr_dep')

utf8proc_base_dep = subproject('utf8proc').get_variable('utf8proc_dep')
utf8proc_dep = declare_dependency(
  dependencies: [utf8proc_base_dep],
  compile_args: get_option('default_library') == 'static' ? ['-DUTF8PROC_STATIC'] : [],
)

lmdb_dep = subproject('lmdb', default_options: [
  'warning_level=0',
  'programs=disabled'
]).get_variable('lmdb_dep')

nghttp2_dep = subproject('nghttp2', default_options: [
  'warning_level=0'
]).get_variable('libnghttp2_dep')

libffi_dep = subproject('libffi', default_options: [
  'warning_level=0',
  'tests=false'
]).get_variable('ffi_dep')

pcre2_dep = subproject('pcre2', default_options: [
  'optimization=s',
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
  build_by_default: true,
  build_always_stale: true
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
  'google-brotli': {
    'dir': 'brotli-1.1.0',
    'inc': 'c/include',
  },
}

subprojects_src = src_root / 'vendor'
subprojects_build = meson.project_build_root() / 'vendor'

lmdb_include_path = subprojects_src / deps_info['lmdb']['dir'] / deps_info['lmdb']['inc']
yyjson_include_path = subprojects_src / deps_info['yyjson']['dir'] / deps_info['yyjson']['inc']
libuv_include_path = subprojects_src / deps_info['libuv']['dir'] / deps_info['libuv']['inc']
llhttp_include_path = subprojects_src / deps_info['llhttp']['dir'] / deps_info['llhttp']['inc']
zlib_include_path = subprojects_build / deps_info['zlib-ng']['dir'] / deps_info['zlib-ng']['inc']
brotli_include_path = subprojects_src / deps_info['google-brotli']['dir'] / deps_info['google-brotli']['inc']

pkg_zig_dir = src_root / 'src' / 'pkg'
pkg_build_dir = meson.current_build_dir()

ant_deps = [
  libffi_dep, crprintf_dep, argtable3_dep,
  llhttp, pcre2_dep, libuv_dep, base64_dep,
  yyjson_dep, minicoro_dep, uuidv7_dep, tlsuv_dep,
  uriparser_dep, utf8proc_dep, ssl_dep, crypto_dep,
  zlib_dep, brotli_common_dep, brotli_dec_dep,
  brotli_enc_dep, uthash_dep, lmdb_dep, wamr_dep,
] + win_deps

if get_option('jit')
  mir_dep = subproject('mir').get_variable('mir_dep')
  ant_deps += [mir_dep]
  add_project_arguments('-DANT_JIT', language: 'c')
endif

if host_machine.system() == 'darwin'
  ant_deps += [security_dep]
endif
