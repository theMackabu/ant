project('ant', ['c', 'cpp'], default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'cpp_std=gnu++23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

python = find_program('python3')
fs = import('fs')
is_static = get_option('static_link')

src_root = meson.project_source_root()
include = include_directories('include')
sandbox_backend_include = include_directories(
  'src/sandbox/backends',
  'src/sandbox/backends/shared/include',
)

sandbox_shared_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:shared',
  check: true
).stdout().strip().split())

sandbox_backend_sources = []
darwin_hvf_supported = host_machine.system() == 'darwin' and host_machine.cpu_family() == 'aarch64'
if host_machine.system() == 'darwin'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin',
    check: true
  ).stdout().strip().split())
  if darwin_hvf_supported
    sandbox_backend_sources += files(run_command(
      python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:darwin_hvf',
      check: true
    ).stdout().strip().split())
    sandbox_backend_sources += sandbox_shared_sources
  endif
elif host_machine.system() == 'linux'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:linux',
    check: true
  ).stdout().strip().split())
  sandbox_backend_sources += sandbox_shared_sources
elif host_machine.system() == 'windows'
  sandbox_backend_sources += files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'sandbox_backend:windows',
    check: true
  ).stdout().strip().split())
endif

subdir('meson')

messages_h = custom_target('messages_h',
  input: 'src/cli/messages.toml',
  output: 'messages.h',
  command: [python, files('meson/messages.py'), '@INPUT@'],
  capture: true,
)

generic_step_h = custom_target('generic_step_h',
  input: 'src/silver/engine.c',
  output: 'generic_step_cases.h',
  command: [python, files('meson/generic_step.py'), '@INPUT@'],
  depend_files: files('meson/generic_step.py'),
  capture: true,
)

theme_h = custom_target('theme_h',
  input: 'src/highlight/theme.toml',
  output: 'theme.h',
  command: [python, files('meson/theme.py'), '@INPUT@'],
  capture: true,
)

lib_sources = files(run_command(
  python, files('meson/sources.py'), files('sources.json'), 'engine',
  check: true
).stdout().strip().split())

section_gc_c_args = host_machine.system() != 'darwin' ? ['-ffunction-sections', '-fdata-sections'] : []

libant = static_library(
  'ant',
  lib_sources + sandbox_backend_sources + [
    builtins_h,
    generic_step_h,
    messages_h,
    theme_h
  ],
  install: true,
  include_directories: [include, sandbox_backend_include],
  c_args: section_gc_c_args,
  dependencies: ant_deps
)

libant_dep = declare_dependency(
  link_with: libant,
  include_directories: [include, sandbox_backend_include],
  dependencies: ant_deps + temporal_link_deps
)

link_args = []
if get_option('static_link')
  link_args += ['-static']
endif

ant_link_args = link_args
if host_machine.system() == 'windows'
  ant_link_args += ['-Wl,--stack,134217728']
elif host_machine.system() == 'darwin'
  ant_link_args += ['-Wl,-stack_size,0x2000000']
else
  ant_link_args += ['-Wl,-z,stack-size=134217728']
endif
if get_option('linker_map')
  ant_linkmap = meson.project_build_root() + '/ant.linkmap'
  if host_machine.system() == 'darwin'
    ant_link_args += ['-Wl,-map,' + ant_linkmap]
  else
    ant_link_args += ['-Wl,-Map=' + ant_linkmap]
  endif
endif

ant_link_depends = []
if host_machine.system() == 'darwin'
  llvm_nm = find_program(get_option('llvm_nm'))
  ant_exports = custom_target('ant_exports',
    input: [libant, libuv_lib],
    output: 'ant.exports',
    command: [python, files('meson/exports.py'), llvm_nm, '@INPUT@'],
    capture: true,
  )
  ant_link_args += [
    '-Wl,-exported_symbols_list,' + ant_exports.full_path(),
    '-Wl,-dead_strip',
  ]
  ant_link_depends += [ant_exports]
endif

if host_machine.system() == 'linux'
  ant_link_args += [
    '-Wl,--gc-sections',
    '-Wl,--dynamic-list=' + meson.current_source_dir() / 'meson/ant.dynlist',
  ]
  ant_link_depends += files('meson/ant.dynlist')
endif

ant_exe = executable(
  'ant',
  files('src/main.c') + [messages_h],
  dependencies: libant_dep,
  link_args: ant_link_args,
  link_depends: ant_link_depends,
  export_dynamic: host_machine.system() == 'windows'
)

if host_machine.system() == 'darwin' and get_option('codesign')
  codesign = find_program('codesign')
  custom_target(
    'ant_codesign',
    input: files('meson/ant.entitlements'),
    output: 'ant.codesign.stamp',
    command: [
      'sh', '-c',
      '"$1" --force --sign - --entitlements "$2" "$3" && touch "$4"',
      'ant-codesign',
      codesign,
      '@INPUT0@',
      ant_exe,
      '@OUTPUT@',
    ],
    depends: ant_exe,
    build_by_default: true,
  )
endif

runtime_binary = get_option('runtime_binary')
if not runtime_binary.disabled()
  runtime_sources = files(run_command(
    python, files('meson/sources.py'), files('sources.json'), 'runtime',
    check: true
  ).stdout().strip().split())

  build_runtime_by_default = runtime_binary.enabled()

  runtime_c_args = []
  ant_runtime_link_args = link_args
  if host_machine.system() == 'windows'
    ant_runtime_link_args += ['-Wl,--stack,134217728']
  elif host_machine.system() == 'darwin'
    ant_runtime_link_args += ['-Wl,-stack_size,0x2000000']
  else
    ant_runtime_link_args += ['-Wl,-z,stack-size=134217728']
  endif
  if host_machine.system() != 'darwin'
    runtime_c_args += ['-ffunction-sections', '-fdata-sections']
    ant_runtime_link_args += ['-Wl,--gc-sections']
  endif

  libant_runtime = static_library(
    'ant-runtime-core',
    runtime_sources + [
      builtins_h,
      generic_step_h,
      messages_h,
      theme_h
    ],
    include_directories: [include],
    c_args: runtime_c_args,
    dependencies: ant_slim_deps,
    build_by_default: build_runtime_by_default,
  )

  ant_runtime_link_depends = []
  if host_machine.system() == 'darwin'
    ant_runtime_exports = custom_target('ant_runtime_exports',
      input: [libant_runtime, libuv_lib],
      output: 'ant-runtime.exports',
      command: [python, files('meson/exports.py'), llvm_nm, '@INPUT@'],
      capture: true,
    )
    ant_runtime_link_args += [
      '-Wl,-exported_symbols_list,' + ant_runtime_exports.full_path(),
      '-Wl,-dead_strip',
    ]
    ant_runtime_link_depends += [ant_runtime_exports]
  elif host_machine.system() == 'linux'
    ant_runtime_link_args += [
      '-Wl,--dynamic-list=' + meson.current_source_dir() / 'meson/ant.dynlist',
    ]
    ant_runtime_link_depends += files('meson/ant.dynlist')
  endif

  ant_runtime_exe = executable(
    'ant-runtime',
    files('src/main_runtime.c') + [messages_h],
    dependencies: declare_dependency(
      link_with: libant_runtime,
      include_directories: [include],
      dependencies: ant_slim_deps + temporal_link_deps
    ),
    link_args: ant_runtime_link_args,
    link_depends: ant_runtime_link_depends,
    export_dynamic: false,
    build_by_default: build_runtime_by_default,
  )

  if host_machine.system() == 'darwin' and get_option('codesign')
    custom_target(
      'ant_runtime_codesign',
      output: 'ant-runtime.codesign.stamp',
      command: [
        'sh', '-c',
        '"$1" -x "$2" && "$3" --force --sign - "$2" && touch "$4"',
        'ant-runtime-codesign',
        find_program('strip'),
        ant_runtime_exe,
        codesign,
        '@OUTPUT@',
      ],
      depends: ant_runtime_exe,
      build_by_default: build_runtime_by_default,
    )
  endif
endif

# move to meson/tests
shape_transition_test = executable(
  'test-shape-transition-identity',
  files('tests/test_shape_transition_identity.c', 'src/shapes.c'),
  include_directories: include,
  dependencies: uthash_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('shape-transition-identity', shape_transition_test)

shape_oom_test = executable(
  'test-shape-descriptor-oom',
  files('tests/test_shape_descriptor_oom.c'),
  include_directories: include,
  dependencies: uthash_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('shape-descriptor-oom', shape_oom_test)

opt_ir_sources = files(
  'src/jit/opt/arena.c',
  'src/jit/opt/bitset.c',
  'src/jit/opt/code.c',
  'src/jit/opt/exits.c',
  'src/jit/opt/hash.c',
  'src/jit/opt/ir/ir.c',
  'src/jit/opt/ir/ir_dump.c',
  'src/jit/opt/ir/ir_value.c',
)

opt_passes_sources = files(
  'src/jit/opt/passes/pass_bounds_check_elimination.c',
  'src/jit/opt/passes/pass_boxed_arithmetic.c',
  'src/jit/opt/passes/pass_branch_fusion.c',
  'src/jit/opt/passes/pass_check_elimination.c',
  'src/jit/opt/passes/pass_dce.c',
  'src/jit/opt/passes/pass_dominators.c',
  'src/jit/opt/passes/pass_edit.c',
  'src/jit/opt/passes/pass_escape_analysis.c',
  'src/jit/opt/passes/pass_facts.c',
  'src/jit/opt/passes/pass_frame_initialization.c',
  'src/jit/opt/passes/pass_gvn.c',
  'src/jit/opt/passes/pass_licm.c',
  'src/jit/opt/passes/pass_optimize.c',
  'src/jit/opt/passes/pass_representation.c',
  'src/jit/opt/passes/pass_simplify.c',
  'src/jit/opt/passes/pass_truncation.c',
  'src/jit/opt/passes/pass_verify.c',
)

opt_ir_test = executable(
  'test-opt-ir',
  files('tests/test_opt_ir.c') + opt_ir_sources,
  include_directories: include,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('opt-ir', opt_ir_test)

opt_passes_test = executable(
  'test-opt-passes',
  files('tests/test_opt_passes.c') + opt_ir_sources + opt_passes_sources,
  include_directories: include,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('opt-passes', opt_passes_test)

opt_regalloc_test = executable(
  'test-opt-regalloc',
  files(
    'tests/test_opt_regalloc.c',
    'src/jit/opt/target.c',
    'src/jit/opt/regalloc/ra_allocate.c',
    'src/jit/opt/regalloc/ra_coalescing.c',
    'src/jit/opt/regalloc/ra_liveness.c',
    'src/jit/opt/regalloc/ra_moves.c',
    'src/jit/opt/regalloc/ra_verify.c',
  ) + opt_ir_sources + opt_passes_sources,
  include_directories: include,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)

test('opt-regalloc', opt_regalloc_test, timeout: 300)
opt_asm_include = include_directories('src/jit/opt/asm')

opt_asm_encode_test = executable(
  'test-opt-asm-encode',
  files(
    'tests/test_opt_asm_encode.c',
    'src/jit/opt/asm/buffer.c',
    'src/jit/opt/asm/x64.c',
    'src/jit/opt/asm/masm_x64.c',
    'src/jit/opt/asm/a64.c',
    'src/jit/opt/asm/masm_a64.c',
    'src/jit/opt/asm/disasm.c',
  ),
  include_directories: [include, opt_asm_include],
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('opt-asm-encode', opt_asm_encode_test)

opt_asm_exec_test = executable(
  'test-opt-asm-exec',
  files(
    'tests/test_opt_asm_exec.c',
    'src/jit/opt/asm/buffer.c',
    'src/jit/opt/asm/x64.c',
    'src/jit/opt/asm/masm_x64.c',
    'src/jit/opt/asm/a64.c',
    'src/jit/opt/asm/masm_a64.c',
    'src/jit/opt/asm/executable.c',
  ),
  include_directories: [include, opt_asm_include],
  dependencies: cc.find_library('m', required: false),
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('opt-asm-exec', opt_asm_exec_test)

keyed_store_test = executable(
  'test-keyed-store-policy',
  files('tests/test_keyed_store_policy.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('keyed-store-policy', keyed_store_test)

json_layout_test = executable(
  'test-json-layout-cache',
  files('tests/test_json_layout_cache.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('json-layout-cache', json_layout_test)

array_dense_length_test = executable(
  'test-array-dense-length',
  files('tests/test_array_dense_length.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('array-dense-length', array_dense_length_test)

typedarray_from_cleanup_test = executable(
  'test-typedarray-from-cleanup',
  files('tests/test_typedarray_from_cleanup.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('typedarray-from-cleanup', typedarray_from_cleanup_test)

symbol_iterator_isolates_test = executable(
  'test-symbol-iterator-isolates',
  files('tests/test_symbol_iterator_isolates.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('symbol-iterator-isolates', symbol_iterator_isolates_test)

isolate_libraries_test = executable(
  'test-isolate-libraries',
  files('tests/test_isolate_libraries.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-libraries', isolate_libraries_test)

isolate_threads_test = executable(
  'test-isolate-threads',
  files('tests/test_isolate_threads.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-threads', isolate_threads_test, timeout: 300)

isolate_teardown_test = executable(
  'test-isolate-teardown',
  files('tests/test_isolate_teardown.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-teardown', isolate_teardown_test, timeout: 300, workdir: meson.project_source_root())

error_handoffs_test = executable(
  'test-error-handoffs',
  files('tests/test_error_handoffs.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('error-handoffs', error_handoffs_test)

if host_machine.system() == 'darwin'
  desktop_error_details_test = executable(
    'test-desktop-error-details',
    files('tests/test_desktop_error_details.c'),
    dependencies: [libant_dep, dependency('appleframeworks', modules: 'AppKit')],
    c_args: ['-UNDEBUG', '-x', 'objective-c', '-fobjc-arc'],
    link_args: ['-Wl,-dead_strip'],
    build_by_default: false
  )
  test('desktop-error-details', desktop_error_details_test)
endif

gc_plain_arrays_test = executable(
  'test-gc-plain-arrays',
  files('tests/test_gc_plain_arrays.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-plain-arrays', gc_plain_arrays_test)

gc_shape_refs_test = executable(
  'test-gc-shape-refs',
  files('tests/test_gc_shape_refs.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-shape-refs', gc_shape_refs_test)

gc_exact_edges_test = executable(
  'test-gc-exact-edges',
  files('tests/test_gc_exact_edges.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-exact-edges', gc_exact_edges_test)

gc_untrusted_string_words_test = executable(
  'test-gc-untrusted-string-words',
  files('tests/test_gc_untrusted_string_words.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-untrusted-string-words', gc_untrusted_string_words_test)

gc_pin_permanent_oom_test = executable(
  'test-gc-pin-permanent-oom',
  files('tests/test_gc_pin_permanent_oom.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-pin-permanent-oom', gc_pin_permanent_oom_test)

gc_rope_table_oom_test = executable(
  'test-gc-rope-table-oom',
  files('tests/test_gc_rope_table_oom.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-rope-table-oom', gc_rope_table_oom_test)

jit_entry_stub_test = executable(
  'test-jit-entry-stub',
  files('tests/test_jit_entry_stub.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('jit-entry-stub', jit_entry_stub_test)

gc_jit_entry_saves_test = executable(
  'test-gc-jit-entry-saves',
  files('tests/test_gc_jit_entry_saves.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-jit-entry-saves', gc_jit_entry_saves_test)

gc_uv_segments_test = executable(
  'test-gc-uv-segments',
  files('tests/test_gc_uv_segments.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-uv-segments', gc_uv_segments_test)

gc_array_cards_test = executable(
  'test-gc-array-cards',
  files('tests/test_gc_array_cards.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-array-cards', gc_array_cards_test)

code_unit_feedback_test = executable(
  'test-code-unit-feedback',
  files('tests/test_code_unit_feedback.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('code-unit-feedback', code_unit_feedback_test)

code_unit_lifetime_test = executable(
  'test-code-unit-lifetime',
  files('tests/test_code_unit_lifetime.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('code-unit-lifetime', code_unit_lifetime_test)

code_unit_ctor_proto_test = executable(
  'test-code-unit-ctor-proto',
  files('tests/test_code_unit_ctor_proto.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('code-unit-ctor-proto', code_unit_ctor_proto_test)

ic_poly_shape_release_test = executable(
  'test-ic-poly-shape-release',
  files('tests/test_ic_poly_shape_release.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('ic-poly-shape-release', ic_poly_shape_release_test)

gc_two_isolates_test = executable(
  'test-gc-two-isolates',
  files('tests/test_gc_two_isolates.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('gc-two-isolates', gc_two_isolates_test)

arena_chunks_test = executable(
  'test-arena-chunks',
  files('tests/test_arena_chunks.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('arena-chunks', arena_chunks_test)

isolate_code_arenas_test = executable(
  'test-isolate-code-arenas',
  files('tests/test_isolate_code_arenas.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-code-arenas', isolate_code_arenas_test)

isolate_ic_epochs_test = executable(
  'test-isolate-ic-epochs',
  files('tests/test_isolate_ic_epochs.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-ic-epochs', isolate_ic_epochs_test)

isolate_timers_test = executable(
  'test-isolate-timers',
  files('tests/test_isolate_timers.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('isolate-timers', isolate_timers_test)

ic_identity_wrap_test = executable(
  'test-ic-identity-wrap',
  files('tests/test_ic_identity_wrap.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('ic-identity-wrap', ic_identity_wrap_test)

upvalue_close_order_test = executable(
  'test-upvalue-close-order',
  files('tests/test_upvalue_close_order.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('upvalue-close-order', upvalue_close_order_test)

arguments_storage_test = executable(
  'test-arguments-storage',
  files('tests/test_arguments_storage.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('arguments-storage', arguments_storage_test)

regexp_brand_storage_test = executable(
  'test-regexp-brand-storage',
  files('tests/test_regexp_brand_storage.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('regexp-brand-storage', regexp_brand_storage_test)

rope_mark_test = executable(
  'test-rope-mark',
  files('tests/test_rope_mark.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('rope-mark', rope_mark_test)

jit_inline_eligibility_test = executable(
  'test-jit-inline-eligibility',
  files('tests/test_jit_inline_eligibility.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('jit-inline-eligibility', jit_inline_eligibility_test)

jit_property_ic_test = executable(
  'test-jit-property-ic',
  files('tests/test_jit_property_ic.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('jit-property-ic', jit_property_ic_test)

jit_address_modes_test = executable(
  'test-jit-address-modes',
  files('tests/test_jit_address_modes.c'),
  dependencies: libant_dep,
  c_args: ['-UNDEBUG'],
  build_by_default: false
)
test('jit-address-modes', jit_address_modes_test)

if host_machine.system() != 'windows'
  ascii_scan_test = executable(
    'test-string-ascii-scan',
    files('tests/test_string_ascii_scan.c'),
    include_directories: include,
    c_args: ['-UNDEBUG'],
    build_by_default: false
  )
  test('string-ascii-scan', ascii_scan_test)

  executable(
    'test-listener-accept-burst',
    files('tests/test_listener_accept_burst.c'),
    dependencies: libuv_dep,
    c_args: ['-UNDEBUG'],
    build_by_default: false
  )
endif

executable(
  'tlsuv-boringssl-smoke',
  files('tests/tlsuv_boringssl_smoke.c'),
  dependencies: [tlsuv_dep, libuv_dep, llhttp, zlib_dep],
  build_by_default: false
)

executable(
  'test-websocket-connect-cancel',
  files('tests/test_websocket_connect_cancel.c'),
  dependencies: [tlsuv_dep, libuv_dep, llhttp, zlib_dep],
  build_by_default: false
)

executable(
  'bench-allocator',
  files('tests/bench-allocator.c'),
  dependencies: [dependency('threads')] + allocator_deps,
  build_by_default: false
)

http_protocols_test = executable(
  'test-http-protocols',
  files('tests/test_http_protocols.c'),
  dependencies: libant_dep,
  build_by_default: false
)
test('http-protocols', http_protocols_test)

embed_example = get_option('embed_example')
embed_example_dist = 'packages/libant/dist'

if not embed_example.disabled()
  if fs.is_dir(meson.project_source_root() / embed_example_dist)
    executable(
      'embed-example',
      files('examples/embed/embed.c'),
      include_directories: include_directories(embed_example_dist),
      dependencies: libant_dep,
      build_by_default: false
    )
  elif embed_example.enabled()
    error('embed_example requires ' + embed_example_dist + '; run packages/libant/build.sh first')
  endif
endif

if darwin_hvf_supported
  executable(
    'test-sandbox-net-packets',
    files('tests/test_sandbox_net_packets.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-virtio-blk',
    files('tests/test_sandbox_virtio_blk.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-virtio-rng',
    files('tests/test_sandbox_virtio_rng.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-pci',
    files('tests/test_sandbox_pci.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-mmio',
    files('tests/test_sandbox_mmio.c'),
    dependencies: libant_dep,
    build_by_default: false
  )

  executable(
    'test-sandbox-vsock-queue',
    files('tests/test_sandbox_vsock_queue.c'),
    dependencies: libant_dep,
    build_by_default: false
  )
endif

if darwin_hvf_supported or host_machine.system() == 'linux'
  executable(
    'test-sandbox-9p',
    files('tests/test_sandbox_9p.c'),
    dependencies: libant_dep,
    build_by_default: false
  )
endif
