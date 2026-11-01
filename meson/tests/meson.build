tests_root = meson.project_source_root() / 'tests'
src_root = meson.project_source_root() / 'src'
test_c_args = ['-UNDEBUG']

libant_tests = [
  'keyed-store-policy',
  'json-layout-cache',
  'array-dense-length',
  'typedarray-from-cleanup',
  'symbol-iterator-isolates',
  'isolate-libraries',
  'isolate-threads',
  'isolate-teardown',
  'error-handoffs',
  'gc-plain-arrays',
  'gc-shape-refs',
  'gc-exact-edges',
  'gc-untrusted-string-words',
  'gc-pin-permanent-oom',
  'gc-rope-table-oom',
  'jit-entry-stub',
  'gc-jit-entry-saves',
  'gc-uv-segments',
  'gc-array-cards',
  'code-unit-feedback',
  'code-unit-lifetime',
  'code-unit-ctor-proto',
  'ic-poly-shape-release',
  'gc-two-isolates',
  'arena-chunks',
  'isolate-code-arenas',
  'isolate-ic-epochs',
  'isolate-timers',
  'ic-identity-wrap',
  'upvalue-close-order',
  'arguments-storage',
  'regexp-brand-storage',
  'rope-mark',
  'jit-inline-eligibility',
  'jit-property-ic',
  'jit-address-modes',
]

libant_test_timeouts = {
  'isolate-threads': 300,
  'isolate-teardown': 300,
}

foreach name : libant_tests
  exe = executable(
    'test-' + name,
    files(tests_root / 'test_' + name.replace('-', '_') + '.c'),
    dependencies: libant_dep,
    c_args: test_c_args,
    build_by_default: false
  )
  timeout = libant_test_timeouts.get(name, 30)
  if name == 'isolate-teardown'
    test(name, exe, timeout: timeout, workdir: meson.project_source_root())
  else
    test(name, exe, timeout: timeout)
  endif
endforeach

shape_transition_test = executable(
  'test-shape-transition-identity',
  files(tests_root / 'test_shape_transition_identity.c', src_root / 'shapes.c'),
  include_directories: include,
  dependencies: uthash_dep,
  c_args: test_c_args,
  build_by_default: false
)
test('shape-transition-identity', shape_transition_test)

shape_oom_test = executable(
  'test-shape-descriptor-oom',
  files(tests_root / 'test_shape_descriptor_oom.c'),
  include_directories: include,
  dependencies: uthash_dep,
  c_args: test_c_args,
  build_by_default: false
)
test('shape-descriptor-oom', shape_oom_test)

opt_root = src_root / 'jit/opt'

opt_ir_sources = files(
  opt_root / 'arena.c',
  opt_root / 'bitset.c',
  opt_root / 'code.c',
  opt_root / 'exits.c',
  opt_root / 'hash.c',
  opt_root / 'ir/ir.c',
  opt_root / 'ir/ir_dump.c',
  opt_root / 'ir/ir_value.c',
)

opt_passes_sources = files(
  opt_root / 'passes/pass_bounds_check_elimination.c',
  opt_root / 'passes/pass_boxed_arithmetic.c',
  opt_root / 'passes/pass_branch_fusion.c',
  opt_root / 'passes/pass_check_elimination.c',
  opt_root / 'passes/pass_dce.c',
  opt_root / 'passes/pass_dominators.c',
  opt_root / 'passes/pass_edit.c',
  opt_root / 'passes/pass_escape_analysis.c',
  opt_root / 'passes/pass_facts.c',
  opt_root / 'passes/pass_frame_initialization.c',
  opt_root / 'passes/pass_gvn.c',
  opt_root / 'passes/pass_licm.c',
  opt_root / 'passes/pass_optimize.c',
  opt_root / 'passes/pass_representation.c',
  opt_root / 'passes/pass_simplify.c',
  opt_root / 'passes/pass_truncation.c',
  opt_root / 'passes/pass_verify.c',
)

opt_regalloc_sources = files(
  opt_root / 'target.c',
  opt_root / 'regalloc/ra_allocate.c',
  opt_root / 'regalloc/ra_coalescing.c',
  opt_root / 'regalloc/ra_liveness.c',
  opt_root / 'regalloc/ra_moves.c',
  opt_root / 'regalloc/ra_verify.c',
)

opt_asm_sources = files(
  opt_root / 'asm/buffer.c',
  opt_root / 'asm/x64.c',
  opt_root / 'asm/masm_x64.c',
  opt_root / 'asm/a64.c',
  opt_root / 'asm/masm_a64.c',
)

opt_asm_include = include_directories('../../src/jit/opt/asm')

opt_ir_test = executable(
  'test-opt-ir',
  files(tests_root / 'test_opt_ir.c') + opt_ir_sources,
  include_directories: include,
  c_args: test_c_args,
  build_by_default: false
)
test('opt-ir', opt_ir_test)

opt_passes_test = executable(
  'test-opt-passes',
  files(tests_root / 'test_opt_passes.c') + opt_ir_sources + opt_passes_sources,
  include_directories: include,
  c_args: test_c_args,
  build_by_default: false
)
test('opt-passes', opt_passes_test)

opt_regalloc_test = executable(
  'test-opt-regalloc',
  files(tests_root / 'test_opt_regalloc.c') + opt_regalloc_sources + opt_ir_sources + opt_passes_sources,
  include_directories: include,
  c_args: test_c_args,
  build_by_default: false
)
test('opt-regalloc', opt_regalloc_test, timeout: 300)

opt_asm_encode_test = executable(
  'test-opt-asm-encode',
  files(tests_root / 'test_opt_asm_encode.c', opt_root / 'asm/disasm.c') + opt_asm_sources,
  include_directories: [include, opt_asm_include],
  c_args: test_c_args,
  build_by_default: false
)
test('opt-asm-encode', opt_asm_encode_test)

opt_asm_exec_test = executable(
  'test-opt-asm-exec',
  files(tests_root / 'test_opt_asm_exec.c', opt_root / 'asm/executable.c') + opt_asm_sources,
  include_directories: [include, opt_asm_include],
  dependencies: cc.find_library('m', required: false),
  c_args: test_c_args,
  build_by_default: false
)
test('opt-asm-exec', opt_asm_exec_test)

if host_machine.system() == 'darwin'
  desktop_error_details_test = executable(
    'test-desktop-error-details',
    files(tests_root / 'test_desktop_error_details.c'),
    dependencies: [libant_dep, dependency('appleframeworks', modules: 'AppKit')],
    c_args: test_c_args + ['-x', 'objective-c', '-fobjc-arc'],
    link_args: ['-Wl,-dead_strip'],
    build_by_default: false
  )
  test('desktop-error-details', desktop_error_details_test)
endif

if host_machine.system() != 'windows'
  ascii_scan_test = executable(
    'test-string-ascii-scan',
    files(tests_root / 'test_string_ascii_scan.c'),
    include_directories: include,
    c_args: test_c_args,
    build_by_default: false
  )
  test('string-ascii-scan', ascii_scan_test)

  executable(
    'test-listener-accept-burst',
    files(tests_root / 'test_listener_accept_burst.c'),
    dependencies: libuv_dep,
    c_args: test_c_args,
    build_by_default: false
  )
endif

http_protocols_test = executable(
  'test-http-protocols',
  files(tests_root / 'test_http_protocols.c'),
  dependencies: libant_dep,
  build_by_default: false
)
test('http-protocols', http_protocols_test)

executable(
  'tlsuv-boringssl-smoke',
  files(tests_root / 'tlsuv_boringssl_smoke.c'),
  dependencies: [tlsuv_dep, libuv_dep, llhttp, zlib_dep],
  build_by_default: false
)

executable(
  'test-websocket-connect-cancel',
  files(tests_root / 'test_websocket_connect_cancel.c'),
  dependencies: [tlsuv_dep, libuv_dep, llhttp, zlib_dep],
  build_by_default: false
)

executable(
  'bench-allocator',
  files(tests_root / 'bench-allocator.c'),
  dependencies: [dependency('threads')] + allocator_deps,
  build_by_default: false
)

sandbox_tests = []
if darwin_hvf_supported
  sandbox_tests += ['net-packets', 'virtio-blk', 'virtio-rng', 'pci', 'mmio', 'vsock-queue']
endif
if darwin_hvf_supported or host_machine.system() == 'linux'
  sandbox_tests += ['9p']
endif

foreach name : sandbox_tests
  executable(
    'test-sandbox-' + name,
    files(tests_root / 'test_sandbox_' + name.replace('-', '_') + '.c'),
    dependencies: libant_dep,
    build_by_default: false
  )
endforeach

if host_machine.system() != 'windows'
  subdir('fuzz')
endif
