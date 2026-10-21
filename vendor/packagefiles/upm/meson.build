project('upm', version: '1.5.0')

python = find_program('python3', 'python')

upm_src_dir = meson.current_source_dir() / 'src'
upm_sources = files(run_command(
  python, '-c', 'import glob, sys; print(chr(10).join(sorted(glob.glob(sys.argv[1] + "/*.ts"))))', upm_src_dir,
  check: true,
).stdout().strip().split('\n'))
upm_package_json = files('package.json')
