# Fuzz and property targets in tests/fuzz. See tests/fuzz/README.md.
#
# By default each target links tests/fuzz/driver.c and its meson test replays
# the seed corpus plus a fixed set of mutations. With -Dfuzz=enabled (clang)
# the targets link libFuzzer instead and the tests replay the corpus once.
fuzz_root = meson.project_source_root() / 'tests/fuzz'
libfuzzer = get_option('fuzz').enabled()

# known third-party UB, for sanitizer builds
fuzz_env = environment({
  'UBSAN_OPTIONS': 'suppressions=' + fuzz_root / 'ubsan.supp' + ':print_stacktrace=1',
})
# the GC heap is invisible to LeakSanitizer, and its stack scan needs real
# frames (see tests/fuzz/fuzz_js.c)
fuzz_gc_env = environment({
  'UBSAN_OPTIONS': 'suppressions=' + fuzz_root / 'ubsan.supp' + ':print_stacktrace=1',
  'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=0',
})

fuzz_targets = [
  'http1',
  'websocket',
  'numbers',
  'base64',
  'text',
  'regexp',
  'js',
]

foreach name : fuzz_targets
  sources = [fuzz_root / 'fuzz_' + name + '.c']
  if not libfuzzer
    sources += fuzz_root / 'driver.c'
  endif

  exe = executable(
    'fuzz-' + name,
    files(sources),
    dependencies: libant_dep,
    c_args: test_c_args,
    link_args: libfuzzer ? ['-fsanitize=fuzzer'] : [],
    build_by_default: libfuzzer
  )

  corpus = fuzz_root / 'corpus' / name
  test(
    'fuzz-' + name, exe,
    args: libfuzzer ? ['-runs=0', corpus] : [corpus],
    env: name == 'js' ? fuzz_gc_env : fuzz_env,
    suite: 'fuzz',
    timeout: 300
  )
endforeach
