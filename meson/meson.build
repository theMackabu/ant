message('Build configuration:')
message('  buildtype: ' + get_option('buildtype'))
message('  optimization: ' + get_option('optimization'))
message('  debug: ' + (get_option('debug') ? 'true' : 'false'))
message('  b_lto: ' + (get_option('b_lto') ? 'true' : 'false'))
message('  strip: ' + (get_option('strip') ? 'true' : 'false'))

if get_option('buildtype') != 'release'
  warning('Building in non-release mode. For best performance, use --buildtype=release')
endif

subdir('deps')
subdir('version')
subdir('snapshot')
