# Version and build configuration
# Include with: subdir('meson') or subdir('../meson')

git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
if git_hash == ''
  git_hash = 'unknown'
endif

timestamp_opt = get_option('build_timestamp')
if timestamp_opt == ''
  timestamp = run_command('date', '+%s', check: true).stdout().strip()
else
  timestamp = timestamp_opt
endif

version_conf = configuration_data()
version_conf.set('ANT_VERSION', '0.3.13.' + timestamp + '-g' + git_hash)
version_conf.set('ANT_BUILD_TIMESTAMP', timestamp)
version_conf.set('ANT_GIT_HASH', git_hash)

cmd_cc = meson.get_compiler('c')
target_triple = run_command(cmd_cc.cmd_array(), '-dumpmachine', check: true).stdout().strip()

triple_parts = target_triple.split('-')
if triple_parts.length() == 4 and triple_parts[1] != 'unknown' and triple_parts[1] != 'pc' and triple_parts[1] != 'apple'
  target_triple = triple_parts[0] + '-unknown-' + triple_parts[2] + '-' + triple_parts[3]
endif

if tls_lib == 'mbedtls'
  version_conf.set('ANT_TARGET_TRIPLE', target_triple + '-mbedtls')
else
  version_conf.set('ANT_TARGET_TRIPLE', target_triple)
endif

config_h = configure_file(
  input: src_root / 'include' / 'config.h.in',
  output: 'config.h',
  configuration: version_conf
)
