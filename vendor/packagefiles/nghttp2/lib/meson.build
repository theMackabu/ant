subdir('includes')
sources = files(
  'nghttp2_alpn.c',
  'nghttp2_buf.c',
  'nghttp2_callbacks.c',
  'nghttp2_debug.c',
  'nghttp2_extpri.c',
  'nghttp2_frame.c',
  'nghttp2_hd.c',
  'nghttp2_hd_huffman.c',
  'nghttp2_hd_huffman_data.c',
  'nghttp2_helper.c',
  'nghttp2_http.c',
  'nghttp2_map.c',
  'nghttp2_mem.c',
  'nghttp2_option.c',
  'nghttp2_outbound_item.c',
  'nghttp2_pq.c',
  'nghttp2_priority_spec.c',
  'nghttp2_queue.c',
  'nghttp2_ratelim.c',
  'nghttp2_rcbuf.c',
  'nghttp2_session.c',
  'nghttp2_stream.c',
  'nghttp2_submit.c',
  'nghttp2_time.c',
  'nghttp2_version.c',
  'sfparse.c',
)
public_c_args = []
c_args = ['-DBUILDING_NGHTTP2', '-DHAVE_CONFIG_H']
if host_machine.system() == 'windows' and get_option('default_library') == 'static'
  c_args += ['-DNGHTTP2_STATICLIB']
  public_c_args += ['-DNGHTTP2_STATICLIB']
endif
if host_machine.system() == 'windows'
  public_c_args += ['-Dssize_t=int']
  version_rc = configure_file(
    input: 'version.rc.in',
    output: 'version.rc',
    format: 'cmake@',
    configuration: configuration,
  )
  sources += [import('windows').compile_resources(version_rc)]
endif
include_directories = include_directories('includes')
libnghttp2 = library(
  'nghttp2',
  sources,
  c_args: c_args,
  include_directories: [include_directories, include_directories('..')],
  install: true,
  gnu_symbol_visibility: 'hidden',
  version: meson.project_version(),
)
import('pkgconfig').generate(
  libnghttp2,
  name: 'libnghttp2',
  description: 'HTTP/2 C library',
  url: 'https://github.com/tasuhiro-t/nghttp2',
  extra_cflags: public_c_args,
)
libnghttp2_dep = declare_dependency(
  compile_args: public_c_args,
  include_directories: include_directories,
  link_with: libnghttp2,
)
meson.override_dependency('libnghttp2', libnghttp2_dep)
