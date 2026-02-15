configure_file(
  input: 'nghttp2ver.h.in',
  output: 'nghttp2ver.h',
  format: 'cmake@',
  configuration: configuration,
  install: true,
  install_dir: get_option('includedir') / 'nghttp2',
)

configure_file(
  input: 'nghttp2.h',
  output: 'nghttp2.h',
  copy: true,
  install: true,
  install_dir: get_option('includedir') / 'nghttp2',
)
