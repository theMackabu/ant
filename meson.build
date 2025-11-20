project('ant', 'c', default_options: [
  'optimization=2',
  'c_std=c23'
])

include = include_directories('include')

sources = [
  'src/main.c',
  'src/ant.c',
  'src/modules/server.c',
]

mongoose_proj = subproject('mongoose')
mongoose_dep = mongoose_proj.get_variable('mongoose_dep')

argtable3_proj = subproject('argtable3')
argtable3_dep = argtable3_proj.get_variable('argtable3_dep')

git = find_program('git', required: false)
if git.found()
  git_commit = run_command(git, 'rev-parse', '--short=10', 'HEAD', check: false)
  if git_commit.returncode() == 0
    git_hash = git_commit.stdout().strip()
  else
    git_hash = 'unknown'
  endif
else
  git_hash = 'unknown'
endif

build_date = run_command('date', '+%Y-%m-%d', check: true).stdout().strip()

version_conf = configuration_data()
version_conf.set('ANT_VERSION', '0.0.3')
version_conf.set('ANT_GIT_HASH', git_hash)
version_conf.set('ANT_BUILD_DATE', build_date)

config_h = configure_file(
  input: 'include/config.h.in',
  output: 'config.h',
  configuration: version_conf
)

build_include = include_directories('.')

add_project_arguments('-D JS_DUMP', language: 'c')
executable('ant', sources, include_directories: [include, build_include], dependencies: [mongoose_dep, argtable3_dep])
