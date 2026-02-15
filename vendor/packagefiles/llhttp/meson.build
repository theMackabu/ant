project('llhttp', 'c', version: '9.3.1', default_options: ['c_std=c99', 'warning_level=0'])

llhttp_lib = static_library(
  'llhttp',
  files('src/llhttp.c', 'src/http.c', 'src/api.c'),
  include_directories: include_directories('include'),
)

llhttp_dep = declare_dependency(
  link_with: llhttp_lib,
  include_directories: include_directories('include'),
)
