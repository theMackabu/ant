node = find_program('node')
abi_json = files('abi.json')
abi_value = files('scripts/abi-value.mjs')

initial_memory = run_command(node, abi_value, abi_json, 'INITIAL_MEMORY', check: true).stdout().strip()
maximum_memory = run_command(node, abi_value, abi_json, 'MAX_MEMORY_LIMIT', check: true).stdout().strip()

wasm_sources = files(run_command(
  python, src_root / 'meson/sources.py', src_root / 'sources.json', 'wasm', src_root, '../..',
  check: true,
).stdout().strip().split()) + files(
  'src/bridge.c',
  'src/host_call.c',
  'src/host_stub.c',
  'src/jit_stub.c',
  'src/platform.c',
  'src/response.c',
  'src/stubs.c',
  'src/wire.c',
)

wasm_args = [
  '-DANT_WASM_EMBED=1',
  '-D_WASI_EMULATED_GETPID',
  '-fno-math-errno',
  '-fno-trapping-math',
  '-ffunction-sections',
  '-fdata-sections',
  '-Wno-shorten-64-to-32',
]

executable('ant', wasm_sources, silver_macros_h, unicode_data_h, unicode_id_h,
  include_directories: [include, include_directories('.cache/generated', 'src/include')],
  c_args: wasm_args,
  cpp_args: wasm_args,
  dependencies: ant_deps + rust_link_deps,
  link_args: [
    '-mexec-model=reactor',
    '-Wl,--import-memory',
    '-Wl,--initial-memory=' + initial_memory,
    '-Wl,--max-memory=' + maximum_memory,
    '-Wl,-z,stack-size=1048576',
    '-Wl,--gc-sections',
    '-Wl,--fatal-warnings',
    '-Wl,--error-limit=0',
    '-lwasi-emulated-getpid',
  ],
  name_suffix: 'wasm',
)
