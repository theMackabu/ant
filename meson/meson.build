message('Build configuration:')
message('  buildtype: ' + get_option('buildtype'))
message('  optimization: ' + get_option('optimization'))
message('  debug: ' + (get_option('debug') ? 'true' : 'false'))
message('  b_lto: ' + (get_option('b_lto') ? 'true' : 'false'))
message('  strip: ' + (get_option('strip') ? 'true' : 'false'))

if get_option('buildtype') != 'release'
  warning('Building in non-release mode. For best performance, use --buildtype=release')
endif

add_project_arguments(
  '-DNO_EXECUTE_PERMISSION',
  '-Wshadow',
  '-Wdouble-promotion',
  '-Wformat=2',
  '-Wundef',
  '-Wconversion',
  '-Wno-sign-conversion',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-deprecated-declarations',
  language: 'c'
)

if host_machine.system() == 'linux'
  add_project_arguments('-fno-pie', language: 'c')
  add_project_link_arguments('-no-pie', language: 'c')
endif

subdir('deps')
subdir('version')
subdir('snapshot')
