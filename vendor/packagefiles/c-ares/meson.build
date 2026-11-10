project(
  'c-ares',
  'c',
  version: '1.34.9',
  license: 'MIT',
  meson_version: '>=0.56.0',
)

# don't forget to update mee too
# see: https://github.com/c-ares/c-ares/blob/f1bf69c2d71a7e426ee967847173200a3ef1705a/CMakeLists.txt#L33
libtool_version = '2.19.8'

is_static = (get_option('default_library') == 'static')
is_windows = (host_machine.system() == 'windows')

cc = meson.get_compiler('c')
if is_windows
  winsock_deps = [
    cc.find_library('iphlpapi'),
    cc.find_library('wininet'),
    cc.find_library('ws2_32'),
  ]
else
  winsock_deps = []
endif

cares_dependencies = [winsock_deps]

cares_sources = files(
  'src/lib/dsa/ares_array.c',
  'src/lib/dsa/ares_htable.c',
  'src/lib/dsa/ares_htable_asvp.c',
  'src/lib/dsa/ares_htable_dict.c',
  'src/lib/dsa/ares_htable_strvp.c',
  'src/lib/dsa/ares_htable_szvp.c',
  'src/lib/dsa/ares_htable_vpstr.c',
  'src/lib/dsa/ares_htable_vpvp.c',
  'src/lib/dsa/ares_llist.c',
  'src/lib/dsa/ares_slist.c',
  'src/lib/event/ares_event_configchg.c',
  'src/lib/event/ares_event_epoll.c',
  'src/lib/event/ares_event_kqueue.c',
  'src/lib/event/ares_event_poll.c',
  'src/lib/event/ares_event_select.c',
  'src/lib/event/ares_event_thread.c',
  'src/lib/event/ares_event_wake_pipe.c',
  'src/lib/event/ares_event_win32.c',
  'src/lib/legacy/ares_create_query.c',
  'src/lib/legacy/ares_expand_name.c',
  'src/lib/legacy/ares_expand_string.c',
  'src/lib/legacy/ares_fds.c',
  'src/lib/legacy/ares_getsock.c',
  'src/lib/legacy/ares_parse_a_reply.c',
  'src/lib/legacy/ares_parse_aaaa_reply.c',
  'src/lib/legacy/ares_parse_caa_reply.c',
  'src/lib/legacy/ares_parse_mx_reply.c',
  'src/lib/legacy/ares_parse_naptr_reply.c',
  'src/lib/legacy/ares_parse_ns_reply.c',
  'src/lib/legacy/ares_parse_ptr_reply.c',
  'src/lib/legacy/ares_parse_soa_reply.c',
  'src/lib/legacy/ares_parse_srv_reply.c',
  'src/lib/legacy/ares_parse_txt_reply.c',
  'src/lib/legacy/ares_parse_uri_reply.c',
  'src/lib/record/ares_dns_mapping.c',
  'src/lib/record/ares_dns_multistring.c',
  'src/lib/record/ares_dns_name.c',
  'src/lib/record/ares_dns_parse.c',
  'src/lib/record/ares_dns_record.c',
  'src/lib/record/ares_dns_write.c',
  'src/lib/str/ares_buf.c',
  'src/lib/str/ares_str.c',
  'src/lib/str/ares_strsplit.c',
  'src/lib/util/ares_iface_ips.c',
  'src/lib/util/ares_math.c',
  'src/lib/util/ares_rand.c',
  'src/lib/util/ares_threads.c',
  'src/lib/util/ares_timeval.c',
  'src/lib/util/ares_uri.c',
  'src/lib/ares_addrinfo2hostent.c',
  'src/lib/ares_addrinfo_localhost.c',
  'src/lib/ares_android.c',
  'src/lib/ares_cancel.c',
  'src/lib/ares_close_sockets.c',
  'src/lib/ares_conn.c',
  'src/lib/ares_cookie.c',
  'src/lib/ares_data.c',
  'src/lib/ares_destroy.c',
  'src/lib/ares_free_hostent.c',
  'src/lib/ares_free_string.c',
  'src/lib/ares_freeaddrinfo.c',
  'src/lib/ares_getaddrinfo.c',
  'src/lib/ares_getenv.c',
  'src/lib/ares_gethostbyaddr.c',
  'src/lib/ares_gethostbyname.c',
  'src/lib/ares_getnameinfo.c',
  'src/lib/ares_hosts_file.c',
  'src/lib/ares_init.c',
  'src/lib/ares_library_init.c',
  'src/lib/ares_metrics.c',
  'src/lib/ares_options.c',
  'src/lib/ares_parse_into_addrinfo.c',
  'src/lib/ares_process.c',
  'src/lib/ares_qcache.c',
  'src/lib/ares_query.c',
  'src/lib/ares_search.c',
  'src/lib/ares_send.c',
  'src/lib/ares_set_socket_functions.c',
  'src/lib/ares_socket.c',
  'src/lib/ares_sortaddrinfo.c',
  'src/lib/ares_strerror.c',
  'src/lib/ares_sysconfig.c',
  'src/lib/ares_sysconfig_files.c',
  'src/lib/ares_sysconfig_mac.c',
  'src/lib/ares_sysconfig_win.c',
  'src/lib/ares_timeout.c',
  'src/lib/ares_update_servers.c',
  'src/lib/ares_version.c',
  'src/lib/inet_net_pton.c',
  'src/lib/inet_ntop.c',
  'src/lib/windows_port.c',
)

cares_headers = files(
  'src/lib/include/ares_array.h',
  'src/lib/include/ares_buf.h',
  'src/lib/include/ares_htable_asvp.h',
  'src/lib/include/ares_htable_dict.h',
  'src/lib/include/ares_htable_strvp.h',
  'src/lib/include/ares_htable_szvp.h',
  'src/lib/include/ares_htable_vpstr.h',
  'src/lib/include/ares_htable_vpvp.h',
  'src/lib/include/ares_llist.h',
  'src/lib/include/ares_mem.h',
  'src/lib/include/ares_str.h',
)

cares_include_dirs = include_directories(
  'include',
  'src/lib',
  'src/lib/include',
  'src/lib/dsa',
  'src/lib/record',
)

has_header_options = {
  'HAVE_ARPA_INET_H': cc.has_header('arpa/inet.h'),
  'HAVE_AVAILABILITYMACROS_H': cc.has_header('AvailabilityMacros.h'),
  'HAVE_ERRNO_H': cc.has_header('errno.h'),
  'HAVE_FCNTL_H': cc.has_header('fcntl.h'),
  'HAVE_IFADDRS_H': cc.has_header('ifaddrs.h'),
  'HAVE_IPHLPAPI_H': cc.has_header('iphlpapi.h'),
  'HAVE_LIMITS_H': cc.has_header('limits.h'),
  'HAVE_NETDB_H': cc.has_header('netdb.h'),
  'HAVE_NETINET_IN_H': cc.has_header('netinet/in.h'),
  'HAVE_NETINET_TCP_H': cc.has_header('netinet/tcp.h'),
  'HAVE_NETIOAPI_H': cc.has_header('netioapi.h'),
  'HAVE_NET_IF_H': cc.has_header('net/if.h'),
  'HAVE_POLL_H': cc.has_header('poll.h'),
  'HAVE_RESOLV_H': cc.has_header('resolv.h'),
  'HAVE_STDBOOL_H': cc.has_header('stdbool.h'),
  'HAVE_STDINT_H': cc.has_header('stdint.h'),
  'HAVE_STRINGS_H': cc.has_header('strings.h'),
  'HAVE_STRING_H': cc.has_header('string.h'),
  'HAVE_SYS_EPOLL_H': cc.has_header('sys/epoll.h'),
  'HAVE_SYS_EVENT_H': cc.has_header('sys/event.h'),
  'HAVE_SYS_FILIO_H': cc.has_header('sys/filio.h'),
  'HAVE_SYS_IOCTL_H': cc.has_header('sys/ioctl.h'),
  'HAVE_SYS_PARAM_H': cc.has_header('sys/param.h'),
  'HAVE_SYS_RANDOM_H': cc.has_header('sys/random.h'),
  'HAVE_SYS_SOCKET_H': cc.has_header('sys/socket.h'),
  'HAVE_SYS_STAT_H': cc.has_header('sys/stat.h'),
  'HAVE_SYS_SYSTEM_PROPERTIES_H': cc.has_header('sys/system_properties.h'),
  'HAVE_SYS_TIME_H': cc.has_header('sys/time.h'),
  'HAVE_SYS_TYPES_H': cc.has_header('sys/types.h'),
  'HAVE_SYS_UIO_H': cc.has_header('sys/uio.h'),
  'HAVE_TCP_H': cc.has_header('tcp.h'),
  'HAVE_TIME_H': cc.has_header('time.h'),
  'HAVE_UNISTD_H': cc.has_header('unistd.h'),
  'HAVE_WINDOWS_H': cc.has_header('windows.h'),
  'HAVE_WINSOCK2_H': cc.has_header('winsock2.h'),
  'HAVE_WS2IPDEF_H': cc.has_header('ws2ipdef.h'),
}

header_options = {
  'HAVE_POLL': cc.has_header_symbol('poll.h', 'poll'),
  'HAVE_PIPE': cc.has_header_symbol('unistd.h', 'pipe'),
  'HAVE_PIPE2': cc.has_header_symbol('unistd.h', 'pipe2'),
  'HAVE_KQUEUE': cc.has_header_symbol('sys/event.h', 'kqueue'),
  'HAVE_EPOLL': cc.has_header_symbol('sys/epoll.h', 'epoll_create1'),
  'TIME_WITH_SYS_TIME': (
    cc.has_header('sys/time.h')
    and cc.has_header('time.h')
  ),
  'HAVE_FCNTL_O_NONBLOCK': cc.has_header_symbol('fcntl.h', 'O_NONBLOCK'),
  'HAVE_STRNCASECMP': cc.has_header_symbol('string.h', 'strncasecmp'),
  'HAVE_STRCASECMP': cc.has_header_symbol('string.h', 'strcasecmp'),
  'HAVE_STRCMPI': cc.has_header_symbol('string.h', 'strcmpi'),
  'HAVE_STRNCMPI': cc.has_header_symbol('string.h', 'strncmpi'),
  'HAVE_STRNICMP': cc.has_header_symbol('string.h', 'strnicmp'),
  'HAVE_STRDUP': cc.has_header_symbol('string.h', 'strdup'),
  'HAVE_CLOSESOCKET': cc.has_header_symbol('winsock.h', 'closesocket'),
  'HAVE_CLOCK_GETTIME_MONOTONIC': cc.has_header_symbol(
    'time.h',
    'CLOCK_MONOTONIC',
  ) and not is_windows,
  'HAVE_GETTIMEOFDAY': cc.has_header_symbol('time.h', 'gettimeofday') and not is_windows,
  'HAVE_RECVFROM': cc.has_function(
    'recvfrom',
    prefix: '#include <sys/types.h>\n#include <sys/socket.h>',
  ),
  'HAVE_WRITEV': cc.has_function(
    'writev',
    prefix: '#include <sys/uio.h>',
  ),
  'HAVE_GETIFADDRS': cc.has_function(
    'getifaddrs',
    prefix: '#include <ifaddrs.h>',
  ),
  # on windows will cause redefinition warnings
  'HAVE_BOOL_T': (cc.has_type(
    'bool',
    prefix: '#include <stdbool.h>',
  ) and not is_windows),
  'HAVE_STRUCT_TIMEVAL': cc.has_type(
    'struct timeval',
    prefix: '#include <sys/time.h>',
  ),
  'HAVE_STRUCT_SOCKADDR_IN6': cc.has_type(
    'struct sockaddr_in6',
    prefix: '#include <sys/socket.h>\n#include <netinet/in.h>',
  ),
  'HAVE_STRUCT_SOCKADDR_IN6_SIN6_SCOPE_ID': cc.has_member(
    'struct sockaddr_in6',
    'sin6_scope_id',
    prefix: '#include <sys/socket.h>\n#include <netinet/in.h>',
  ),
  'HAVE_STRUCT_ADDRINFO': cc.has_type(
    'struct addrinfo',
    prefix: '#include <sys/types.h>\n#include <sys/socket.h>\n#include <netdb.h>',
  ),
  'HAVE_AF_INET6': cc.has_header_symbol(
    'sys/socket.h',
    'AF_INET6',
    prefix: '#include <netinet/in.h>',
  ),
  'HAVE_PF_INET6': cc.has_header_symbol(
    'sys/socket.h',
    'PF_INET6',
    prefix: '#include <netinet/in.h>',
  ),
}

feature_defs = []

foreach def, enabled : header_options + has_header_options
  if (enabled)
    feature_defs += ['-D' + def]
  endif
endforeach

cc = meson.get_compiler('c')
cares_args_map = {
  'public': [is_static ? '-DCARES_STATICLIB' : []],
  'common': feature_defs,
  'linux': [
    '-DHAVE_RECV',
    '-DRECV_TYPE_ARG1=int',
    '-DRECV_TYPE_ARG2=void*',
    '-DRECV_TYPE_ARG3=size_t',
    '-DRECV_TYPE_ARG4=int',
    '-DRECV_TYPE_RETV=ssize_t',
    '-DHAVE_SEND',
    '-DSEND_TYPE_ARG1=int',
    '-DSEND_QUAL_ARG2=const',
    '-DSEND_TYPE_ARG2=void*',
    '-DSEND_TYPE_ARG3=size_t',
    '-DSEND_TYPE_ARG4=int',
    '-DSEND_TYPE_RETV=ssize_t',
    '-DRECVFROM_TYPE_RETV=ssize_t',
    '-DRECVFROM_TYPE_ARG3=size_t',
  ],
  'windows': ['-DRECVFROM_TYPE_RETV=int', '-DRECVFROM_TYPE_ARG3=int'],
}
cares_args_map += {
  'darwin': cares_args_map['linux'],
}

cares_compile_args = [
  cares_args_map['common'],
  cares_args_map.get(host_machine.system(), []),
]

cares_config = {
  'common': {
    'CARES_HAVE_SYS_TYPES_H': cc.has_header('sys/types.h'),
    'CARES_HAVE_SYS_RANDOM_H': cc.has_header('sys/random.h'),
    'CARES_HAVE_SYS_SOCKET_H': cc.has_header('sys/socket.h'),
    'CARES_HAVE_WINDOWS_H': cc.has_header('windows.h'),
    'CARES_HAVE_WS2TCPIP_H': cc.has_header('ws2tcpip.h'),
    'CARES_HAVE_WINSOCK2_H': cc.has_header('winsock2.h'),
    'CARES_HAVE_ARPA_NAMESER_H': cc.has_header('arpa/nameser.h'),
    'CARES_HAVE_ARPA_NAMESER_COMPAT_H': cc.has_header('arpa/nameser_compat.h'),
    'CARES_TYPEOF_ARES_SSIZE_T': 'ssize_t',
    'CARES_TYPEOF_ARES_SOCKLEN_T': 'socklen_t',
  },
  'windows': {
    'CARES_TYPEOF_ARES_SSIZE_T': 'intptr_t',
    'CARES_TYPEOF_ARES_SOCKLEN_T': 'int',
  },
}

ares_build_h = configure_file(
  input: 'include/ares_build.h.cmake',
  output: 'ares_build.h',
  configuration: cares_config['common'] + cares_config.get(
    host_machine.system(),
    {},
  ),
  format: 'cmake@',
)

libcares_kwargs = {
  'sources': [cares_sources, cares_headers, ares_build_h],
  'include_directories': cares_include_dirs,
  'dependencies': cares_dependencies,
}

libcares = library(
  'cares',
  c_args: [
    cares_compile_args,
    '-DCARES_BUILDING_LIBRARY',
    is_static ? '-DCARES_STATICLIB' : [],
  ],
  install: true,
  version: libtool_version,
  kwargs: libcares_kwargs,
)

cares_dep = declare_dependency(
  include_directories: cares_include_dirs,
  link_with: libcares,
  compile_args: cares_args_map['public'],
)
meson.override_dependency('libcares', cares_dep)

if not is_static
  libcares_static = static_library(
    'cares',
    c_args: [
      cares_compile_args,
      '-DCARES_BUILDING_LIBRARY',
      '-DCARES_STATICLIB',
    ],
    build_by_default: false,
    kwargs: libcares_kwargs,
  )

  cares_static_dep = declare_dependency(
    include_directories: cares_include_dirs,
    link_with: libcares_static,
    compile_args: ['-DCARES_STATICLIB'],
  )
else
  cares_static_dep = cares_dep
endif

cares_tools_include_dirs = include_directories('src/tools')

adig = executable(
  'adig',
  sources: files(
    'src/tools/adig.c',
    'src/tools/ares_getopt.c',
    'src/tools/ares_getopt.h',
  ),
  include_directories: cares_tools_include_dirs,
  c_args: cares_compile_args,
  dependencies: [cares_static_dep, winsock_deps],
  build_by_default: get_option('tools'),
)
if get_option('export_adig')
  meson.override_find_program('adig', adig)
endif

ahost = executable(
  'ahost',
  sources: files('src/tools/ahost.c', 'src/tools/ares_getopt.c'),
  include_directories: cares_tools_include_dirs,
  c_args: cares_compile_args,
  dependencies: [cares_static_dep, winsock_deps],
  build_by_default: get_option('tools'),
)
if get_option('export_ahost')
  meson.override_find_program('ahost')
endif

test_sources = files(
  'test/ares-test-init.cc',
  'test/ares-test-internal.cc',
  'test/ares-test-live.cc',
  'test/ares-test-main.cc',
  'test/ares-test-misc.cc',
  'test/ares-test-mock-ai.cc',
  'test/ares-test-mock.cc',
  'test/ares-test-ns.cc',
  'test/ares-test-parse-a.cc',
  'test/ares-test-parse-aaaa.cc',
  'test/ares-test-parse-caa.cc',
  'test/ares-test-parse-mx.cc',
  'test/ares-test-parse-naptr.cc',
  'test/ares-test-parse-ns.cc',
  'test/ares-test-parse-ptr.cc',
  'test/ares-test-parse-soa-any.cc',
  'test/ares-test-parse-soa.cc',
  'test/ares-test-parse-srv.cc',
  'test/ares-test-parse-txt.cc',
  'test/ares-test-parse-uri.cc',
  'test/ares-test-parse.cc',
  'test/ares-test.cc',
  'test/dns-proto-test.cc',
  'test/dns-proto.cc',
)

if get_option('tests')
  add_languages(
    'cpp',
    native: false,
  )

  cxx = meson.get_compiler('cpp')

  cares_test = executable(
    'arestest',
    sources: test_sources,
    # we do not really care if they use deprecated declarations in tests
    cpp_args: cares_compile_args + cxx.get_supported_arguments(
      '-Wno-deprecated-declarations',
      '/wd4996',
    ),
    override_options: 'cpp_std=c++17',
    dependencies: [cares_static_dep, cares_dependencies, dependency('gmock')],
    build_by_default: false,
  )

  test('c-ares-tests', cares_test)
endif
