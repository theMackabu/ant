git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
git_hash = git_hash != '' ? git_hash : 'unknown'

timestamp_opt = get_option('build_timestamp')
timestamp = timestamp_opt != '' ? timestamp_opt : run_command('date', '+%s', check: true).stdout().strip()

ant_version = '0.5.3.' + timestamp + '-g' + git_hash
cmd_cc = meson.get_compiler('c')

target_triple = run_command(cmd_cc.cmd_array(), '-dumpmachine', check: true).stdout().strip()
triple_parts = target_triple.split('-')

if triple_parts.length() == 4 and triple_parts[1] != 'unknown' and triple_parts[1] != 'pc' and triple_parts[1] != 'apple'
  target_triple = triple_parts[0] + '-unknown-' + triple_parts[2] + '-' + triple_parts[3]
endif

target_triple = target_triple + (tls_lib == 'mbedtls' ? '-mbedtls' : '')

version_conf = configuration_data()
version_conf.set_quoted('ANT_VERSION', ant_version)
version_conf.set('ANT_BUILD_TIMESTAMP', timestamp)
version_conf.set_quoted('ANT_GIT_HASH', git_hash)
version_conf.set_quoted('ANT_TARGET_TRIPLE', target_triple)

config_h = configure_file(
  output: 'config.h',
  configuration: version_conf
)

version_include = include_directories('.')