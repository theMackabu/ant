project('ada', 'cpp', 
  version: '3.4.4', 
  default_options: ['cpp_std=c++20', 'warning_level=0']
)

ada_include = include_directories('include', 'src')

ada_lib = static_library(
  'ada',
  'src/ada.cpp',
  include_directories: ada_include,
  cpp_args: ['-DADA_INCLUDE_URL_PATTERN=0'],
)

ada_dep = declare_dependency(
  link_with: ada_lib,
  include_directories: ada_include,
)
