fs = import('fs')

git_hash_opt = get_option('build_git_hash')
git_longhash = git_hash_opt != '' ? git_hash_opt : run_command('git', 'rev-parse', 'HEAD', check: false).stdout().strip()
git_longhash = git_longhash != '' ? git_longhash : 'unknown'
git_hash = git_longhash.substring(0, 8)

timestamp_opt = get_option('build_timestamp')
timestamp = timestamp_opt != '' ? timestamp_opt : run_command('date', '+%s', check: true).stdout().strip()

version_major = ''
version_minor = ''
version_patch = ''

foreach line : fs.read(meson.project_source_root() / 'meson' / 'ant.version').strip().split('\n')
  line = line.strip()
  if line == '' or line.startswith('#')
    continue
  endif

  parts = line.split('=')
  if parts.length() != 2
    error('invalid meson/ant.version line: ' + line)
  endif

  key = parts[0].strip()
  value = parts[1].strip()
  if value == ''
    error('invalid empty meson/ant.version value for ' + key)
  endif
  value_int = value.to_int()
  if value_int < 0
    error('invalid negative meson/ant.version value for ' + key)
  endif

  if key == 'major'
    version_major = value
  elif key == 'minor'
    version_minor = value
  elif key == 'patch'
    version_patch = value
  else
    error('unknown meson/ant.version key: ' + key)
  endif
endforeach

if version_major == '' or version_minor == '' or version_patch == ''
  error('meson/ant.version must define major, minor, and patch')
endif

ant_version = version_major + '.' + version_minor + '.' + git_hash + '.' + version_patch
cmd_cc = meson.get_compiler('c')

target_triple = run_command(cmd_cc.cmd_array(), '-dumpmachine', check: true).stdout().strip()
triple_parts = target_triple.split('-')

if triple_parts.length() == 4 and triple_parts[1] != 'unknown' and triple_parts[1] != 'pc' and triple_parts[1] != 'apple'
  target_triple = triple_parts[0] + '-unknown-' + triple_parts[2] + '-' + triple_parts[3]
endif

add_project_arguments(
  '-DANT_VERSION="' + ant_version + '"',
  '-DANT_BUILD_TIMESTAMP=' + timestamp,
  '-DANT_GIT_HASH="' + git_hash + '"',
  '-DANT_GIT_LONGHASH="' + git_longhash + '"',
  '-DANT_TARGET_TRIPLE="' + target_triple + '"',
  '-DSKIM_VERSION="' + skim_version + '"',
  language: 'c'
)
