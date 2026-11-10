fs = import('fs')

git_hash_opt = get_option('build_git_hash')
git_longhash = git_hash_opt != '' ? git_hash_opt : run_command('git', 'rev-parse', 'HEAD', check: false).stdout().strip()
git_longhash = git_longhash != '' ? git_longhash : 'unknown'
git_hash = git_longhash.substring(0, 8)

timestamp_opt = get_option('build_timestamp')
timestamp = timestamp_opt != '' ? timestamp_opt : run_command('date', '+%s', check: true).stdout().strip()
canary = cc.get_define('ANT_CANARY', args: get_option('c_args')) == '1' ? '1' : '0'

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
  '-DANT_CANARY=' + canary,
  '-DANT_GIT_HASH="' + git_hash + '"',
  '-DANT_GIT_LONGHASH="' + git_longhash + '"',
  '-DANT_TARGET_TRIPLE="' + target_triple + '"',
  language: 'c'
)

if is_wasi
  subdir_done()
endif

dep_versions = {
  'SKIM': skim_dep.version(),
  'UV': libuv_dep.version(),
  'BROTLI': brotli_common_dep.version(),
  'LLHTTP': llhttp.version(),
  'YYJSON': yyjson_dep.version(),
  'PCRE2': pcre2_dep.version(),
  'LIBFFI': libffi_dep.version(),
  'LMDB': lmdb_dep.version(),
  'UTF8PROC': utf8proc_base_dep.version(),
  'ZLIB': zlib_dep.version(),
}

unicode_version = subproject('ucd').get_variable('ucd_version').split('.')
dep_versions += {'UNICODE': unicode_version[0] + '.' + unicode_version[1]}

foreach define, wrap : {'WAMR': 'wasm-micro-runtime', 'BORINGSSL': 'boringssl'}
  revision = ''
  foreach line : fs.read(meson.project_source_root() / 'vendor' / wrap + '.wrap').split('\n')
    if line.startswith('revision')
      revision = line.split('=')[1].strip()
    endif
  endforeach
  dep_versions += {define: revision}
endforeach

foreach define, version : dep_versions
  if version in ['', 'undefined', 'unknown']
    error('could not determine ' + define + ' version')
  endif
  add_project_arguments('-DANT_DEP_' + define + '_VERSION="' + version + '"', language: 'c')
endforeach
