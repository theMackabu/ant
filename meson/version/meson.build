fs = import('fs')
git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()
git_hash = git_hash != '' ? git_hash : 'unknown'

timestamp_opt = get_option('build_timestamp')
timestamp = timestamp_opt != '' ? timestamp_opt : run_command('date', '+%s', check: true).stdout().strip()

ant_base_version = fs.read(meson.project_source_root() / 'meson' / 'ant.version').strip()
ant_version = ant_base_version + '.' + timestamp + '-g' + git_hash
cmd_cc = meson.get_compiler('c')

target_triple = run_command(cmd_cc.cmd_array(), '-dumpmachine', check: true).stdout().strip()
triple_parts = target_triple.split('-')

if triple_parts.length() == 4 and triple_parts[1] != 'unknown' and triple_parts[1] != 'pc' and triple_parts[1] != 'apple'
  target_triple = triple_parts[0] + '-unknown-' + triple_parts[2] + '-' + triple_parts[3]
endif

target_triple = target_triple + (tls_lib == 'mbedtls' ? '-mbedtls' : '')

add_project_arguments(
  '-DANT_VERSION="' + ant_version + '"',
  '-DANT_BUILD_TIMESTAMP=' + timestamp,
  '-DANT_GIT_HASH="' + git_hash + '"',
  '-DANT_TARGET_TRIPLE="' + target_triple + '"',
  language: 'c'
)
