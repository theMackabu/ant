project(
  'nghttp2',
  'c',
  license: 'mit',
  version: '1.68.0',
)
cc = meson.get_compiler('c')

hex = '0123456789abcdef'
project_version_num = '0x' \
    + hex[meson.project_version().split('.')[0].to_int() / 16] \
  + hex[meson.project_version().split('.')[0].to_int() % 16] \
  + hex[meson.project_version().split('.')[1].to_int() / 16] \
  + hex[meson.project_version().split('.')[1].to_int() % 16] \
  + hex[meson.project_version().split('.')[2].to_int() / 16] \
  + hex[meson.project_version().split('.')[2].to_int() % 16]
configuration = configuration_data()
configuration.set('PACKAGE_VERSION', meson.project_version())
configuration.set('PACKAGE_VERSION_NUM', project_version_num)
configuration.set('PROJECT_VERSION', meson.project_version())
configuration.set('PROJECT_VERSION_MAJOR', meson.project_version().split('.')[0])
configuration.set('PROJECT_VERSION_MINOR', meson.project_version().split('.')[1])
configuration.set('PROJECT_VERSION_PATCH', meson.project_version().split('.')[2])
configuration.set('VERSION', meson.project_version())

if cc.has_function_attribute('noreturn')
  configuration.set('HINT_NORETURN', '__attribute__((noreturn))')
else
  configuration.set('HINT_NORETURN', '')
endif
if not cc.has_header_symbol('sys/types.h', 'ssize_t')
  configuration.set('ssize_t', 'int')
endif
configuration.set('HAVE_STD_MAP_EMPLACE', true)
configuration.set('HAVE_JANNSON', false)
configuration.set('HAVE_LIBXML2', false)
configuration.set('HAVE_MRUBY', false)
configuration.set('HAVE_NEVERBLEED', false)
configuration.set('SIZEOF_INT_P', cc.sizeof('int *'))
configuration.set(
  'SIZEOF_TIME_T',
  cc.sizeof(
    'time_t',
    prefix: '#include <time.h>',
  ),
)

functions = ['_Exit', 'accept4', 'clock_gettime', 'mkostemp', 'pipe2']

foreach f : functions
  configuration.set('HAVE_@0@'.format(f.to_upper()), cc.has_function(f))
endforeach

configuration.set(
  'HAVE_GETTICKCOUNT64',
  cc.has_header_symbol('sysinfoapi.h', 'GetTickCount64'),
)
configuration.set(
  'HAVE_DECL_INITGROUPS',
  cc.has_header_symbol('grp.h', 'initgroups'),
)
configuration.set(
  'HAVE_DECL_CLOCK_MONOTONIC',
  cc.has_header_symbol('time.h', 'CLOCK_MONOTONIC'),
)
configuration.set('DEBUGBUILD', false)
configuration.set('NOTHREADS', false)

headers = [
  'arpa/inet.h',
  'fcntl.h',
  'inttypes.h',
  'limits.h',
  'netdb.h',
  'netinet/in.h',
  'netinet/ip.h',
  'pwd.h',
  'sys/socket.h',
  'sys/time.h',
  'syslog.h',
  'unistd.h',
  'windows.h',
]

foreach h : headers
  configuration.set(
    'HAVE_@0@'.format(h.underscorify().to_upper()),
    cc.has_header(h),
  )
endforeach

configuration.set('HAVE_LIBBPF', false)
configuration.set(
  'HAVE_BPF_STATS_TYPE',
  cc.has_header_symbol('linux/bpf.h', 'enum bpf_stats_type'),
)
configuration.set('HAVE_LIBNGTCP2_CRYPTO_QUICTLS', false)
configuration.set('HAVE_LIBEV', false)

configure_file(
  input: 'cmakeconfig.h.in',
  output: 'config.h',
  format: 'cmake@',
  configuration: configuration,
)

subdir('lib')
