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
  boringssl_opts.append_compile_args(lang,
    '-w', '-g0', '-DOPENSSL_SMALL', '-DOPENSSL_NO_ASM', '-DOPENSSL_NO_SOCK',
    '-D_WASI_EMULATED_GETPID',
    '-DOPENSSL_NO_THREADS_CORRUPT_MEMORY_AND_LEAK_SECRETS_IF_THREADED',
  )
endforeach
boringssl_opts.set_override_option('warning_level', '0')
crypto_dep = cmake.subproject('boringssl', options: boringssl_opts).dependency('crypto')

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

base64_dep = declare_dependency(
  dependencies: subproject('aklomp-base64').get_variable('base64_dep'),
  compile_args: ['-DBASE64_STATIC_DEFINE'],
)

utf8proc_base_dep = subproject('utf8proc', default_options: ['tests=disabled',]).get_variable('utf8proc_dep')
utf8proc_srcdir = src_root / 'vendor' / ('utf8proc-' + utf8proc_base_dep.version())

utf8proc_dep = declare_dependency(
  dependencies: [utf8proc_base_dep],
  compile_args: ['-DUTF8PROC_STATIC'],
)

pcre2_dep = subproject('pcre2', default_options: [
  'optimization=3',
  'warning_level=0',
  'grep=false',
  'jit=disabled',
  'test=false',
]).get_variable('libpcre2_8')

ucd_derived_core_properties = subproject('ucd').get_variable('derived_core_properties')

ant_deps = [
  base64_dep,
  pcre2_dep,
  utf8proc_dep,
  zlib_dep,
  crypto_dep,
  
  brotli_sub.get_variable('brotli_common_dep'),
  brotli_sub.get_variable('brotli_decoder_dep'),
  brotli_sub.get_variable('brotli_encoder_dep'),
  
  subproject('uthash').get_variable('uthash_dep'),
  subproject('crprintf').get_variable('crprintf_dep'),
  subproject('double-conversion').get_variable('double_conversion_dep'),
  subproject('dragonbox').get_variable('dragonbox_dep'),
  subproject('yyjson', default_options: ['non-standard-json=disabled']).get_variable('yyjson_dep'),
  subproject('ada').get_variable('ada_dep'),
  subproject('uuidv7').get_variable('uuidv7_dep'),
]
