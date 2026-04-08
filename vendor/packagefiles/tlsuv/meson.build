project('tlsuv', 'c', version: '0.40.13', default_options: ['c_std=c11', 'warning_level=0'])

cc = meson.get_compiler('c')
tls_backend = get_option('tls_backend')
cmake_prefix = get_option('deps_prefix_cmake')

libuv_dep = dependency('libuv', required: false)
if not libuv_dep.found()
  libuv_dep = subproject('libuv').get_variable('libuv_dep')
endif

llhttp_dep = dependency('llhttp', required: false)
if not llhttp_dep.found()
  llhttp_dep = subproject('llhttp').get_variable('llhttp_dep')
endif

zlib_dep = dependency('zlib', required: false)
if not zlib_dep.found()
  zlib_dep = subproject('zlib-ng').get_variable('zlib_ng_dep')
endif

tlsuv_sources = files(
  'deps/uv_link_t/src/uv_link_t.c',
  'deps/uv_link_t/src/uv_link_source_t.c',
  'deps/uv_link_t/src/uv_link_observer_t.c',
  'deps/uv_link_t/src/defaults.c',
  'src/tlsuv.c',
  'src/um_debug.c',
  'src/base64.c',
  'src/tls_engine.c',
  'src/p11.c',
  'src/socket.c',
  'src/connector.c',
  'src/url.c',
  'src/alloc.c',
  'src/keychain.c',
  'src/http.c',
  'src/websocket.c',
  'src/http_req.c',
  'src/tls_link.c',
  'src/compression.c',
)

if host_machine.system() == 'darwin'
  tlsuv_sources += files('src/apple/keychain.c')
elif host_machine.system() == 'windows'
  tlsuv_sources += files('src/win32/win32_keychain.c')
endif

tlsuv_sources += files('src/openssl/engine.c', 'src/openssl/keys.c')

tlsuv_c_args = [
  '-DTLSUV_VERSION=v' + meson.project_version(),
  '-DTLSUV_HTTP',
  '-DTLS_IMPL=' + tls_backend,
  '-DUSE_OPENSSL',
  '-w',
]

if cmake_prefix != ''
  tlsuv_c_args += ['-I' + cmake_prefix / 'include']
endif

if host_machine.system() == 'windows'
  tlsuv_c_args += ['-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DWIN32_LEAN_AND_MEAN',
                   '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
                   '-D_WINSOCK_DEPRECATED_NO_WARNINGS']
elif host_machine.system() == 'linux'
  tlsuv_c_args += ['-D_POSIX_C_SOURCE=200112', '-D_GNU_SOURCE']
endif

tls_dep = dependency('openssl', required: true)
tlsuv_c_args += ['-I' + tls_dep.get_variable(pkgconfig: 'includedir')]

win_deps = []
if host_machine.system() == 'windows'
  win_deps = [cc.find_library('crypt32'), cc.find_library('ncrypt')]
endif

config_data = configuration_data()
config_data.set('PROJECT_VERSION_MAJOR', meson.project_version().split('.')[0])
config_data.set('PROJECT_VERSION_MINOR', meson.project_version().split('.')[1])
config_data.set('PROJECT_VERSION_PATCH', meson.project_version().split('.')[2])
config_data.set('tlsuv_VERSION', meson.project_version())
config_data.set('TLSUV_TLSLIB', tls_backend)
config_data.set('TLSUV_HTTP', true)

subdir('generated')

tlsuv_lib = static_library(
  'tlsuv',
  tlsuv_sources, tlsuv_config_h,
  c_args: tlsuv_c_args,
  dependencies: [libuv_dep, llhttp_dep, zlib_dep] + win_deps,
  include_directories: [
    include_directories('include'),
    include_directories('src'),
    include_directories('deps/uv_link_t'),
    include_directories('deps/uv_link_t/include'),
    tlsuv_config_inc,
  ],
)

tlsuv_dep = declare_dependency(
  link_with: tlsuv_lib,
  include_directories: [
    include_directories('include'),
    include_directories('deps/uv_link_t/include'),
    tlsuv_config_inc,
  ],
)
