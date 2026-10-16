# Realms

Status: active
Last reviewed: 2026-10-10
Owner: theMackabu

## Goal

Several ECMAScript realms (a global object plus its intrinsics) sharing one
isolate: one heap, GC, interning, JIT and agent. This is the base for
`node:vm` contexts (`runInNewContext`, `createContext`, `DONT_CONTEXTIFY`),
where a new context sees fresh built-ins: `vm.runInNewContext('Array') !==
Array`. Isolates (separate heaps, for `worker_threads`) are a different,
later piece of work.

## Constraints

- No cost for single-realm programs beyond one compare on call fast paths.
- Keep the 1,500+ `js->sym.*` / `js->builtins.*` / `js->global` access sites
  and the JIT's `offsetof(ant_t, ...)` loads unchanged.
- A Node vm context only gets ECMAScript built-ins (no `console`, `URL`,
  timers, `require`), so a new realm runs only the ECMAScript part of
  bootstrap.

## Design

### Realm state is swapped into `ant_t`

The active realm's values live in their usual `ant_t` fields. An inactive
realm keeps its copies in an `ant_realm_t`. Entering a realm swaps the two
(an X-macro list of fields), so existing code reads the active realm without
an extra load. Per-realm fields:

- `global`, `this_val`, the global lexicals (`global_lexicals`, count, cap,
  index tables).
- The prototype and constructor fields of `js->sym` (`object_proto`,
  `array_proto`, `function_proto`, the primitive, promise, iterator,
  generator protos, `array_ctor`, `string_ctor`, `promise_ctor`). Well-known
  symbols and the `Symbol.for` registry are agent-wide and stay shared.
- The ECMAScript-level `js->builtins` entries (error, RegExp, iterator,
  collection, typed array, Intl, Temporal, wasm protos) and the RegExp
  legacy statics in `mutable_roots`.
- The protector flags and `array_chain_plain_epoch`.

Node-module and web-API `builtins` entries, `modules.*`, `process_state`,
primordials and `Ant` belong to the main realm and are not swapped.

Inactive realms' fields are GC roots.

### Functions belong to a realm

- `sv_func_t` records the realm it was compiled in; closures, generators
  and async coroutines take it from their function. One `sv_func_t` belongs
  to exactly one realm, so per-function caches (ICs, object-literal
  templates, JIT code with baked values) stay single-realm. A `vm.Script`
  run in several contexts compiles once per realm.
- Calls switch realm in C. Errors and suspension come back as return values,
  so a C-scoped save/restore also restores on throw and yield. Switch points:
  `sv_execute_entry_common` and `sv_jit_invoke` (every C-to-interpreter and
  C-to-JIT entry), coroutine activate/deactivate, generator resume, JIT
  compilation (`sv_jit_compile*` compiles under the function's realm).
- Fast paths that push frames or call compiled code directly take only
  same-realm callees and otherwise fall back to the C call: interpreter
  `CALL`, `CALL_METHOD`, `TAIL_CALL`, `TAIL_CALL_METHOD`; JIT direct calls,
  `new`, `apply` forwarding, devirtualized calls; inlining requires
  `callee->realm == caller->realm` at compile time.

### Builtins belong to a realm

Each realm interns its own copies of the C builtins (`kTypeBuiltin`), and a
builtin runs in its own realm whoever calls it, so indirect `eval`,
`Function`, species and Array creation use the builtin's realm. Natives take
the prototypes of the objects they create from per-realm tables filled at
init, never from the global's current bindings.

### Protectors

Protector flags are per realm. A mutation can reach another realm's
intrinsic (objects pass between contexts), so once a second realm exists
invalidation also checks every inactive realm's saved intrinsics and flags.
The check is written once (`ANT_DEFINE_PROTECTOR_INVALIDATE`) and
instantiated for the isolate and for `ant_realm_t`, which name their fields
alike. Arrays and promises can belong to any realm, so a write on one
invalidates in every realm.

### Realm creation

Init is split into agent-level setup (once: intern, pools, VM, well-known
symbols, library registry, roots, snapshot, primordials) and realm-level
intrinsic creation (`isolate_init`'s object block plus the ECMAScript
`init_*` modules), which runs again inside a new realm. Prototypes resolved
lazily against the active realm (`T_NEEDS_PROTO_FALLBACK` objects with an
undefined proto) are set explicitly for objects a realm creates.

### Contextified globals (`node:vm`)

V8 separates a realm's global object from its global proxy, and Node's
contextify puts interceptors on the global object. Ant mirrors the split
with two realm fields instead of making `js->global` exotic (the global ICs,
JIT snapshots, own-property checks and internal slots all assume an
ordinary `js->global`):

- `js->global` stays the realm's ordinary global: built-ins, internal slots,
  `var`/function bindings when the realm is not contextified.
- `js->global_sandbox` is the context object of a contextified realm
  (undefined otherwise). The global variable helpers (`ops/globals.h`,
  `ops/eval_env.h`) consult it in their slow paths: reads try global
  lexicals, then the sandbox (prototype chain included), then the global;
  writes, `var` and function declarations go to the sandbox; deletes remove
  the sandbox's property, else the global's. Global ICs and JIT snapshots do
  not fill in a contextified realm, since the sandbox can shadow any global
  at any time, so every access reaches those helpers.
- `js->global_this` is what `this`, `globalThis` and sloppy `this` see: the
  global itself, or in a contextified realm a proxy whose C traps apply the
  same sandbox-then-global rules to property access (`globalThis.x`, the
  object returned to the outer realm), refuse to be frozen, and report the
  sandbox's keys plus the global's.

Scripts run by `node:vm` use repl-style global declaration (top-level `var`
and functions become global properties; `let`/`const` persist per realm, as
V8's script context table does) with the script parse goal, so top-level
`await` stays a SyntaxError. A `vm.Script` parses when it is constructed
(syntax errors throw there) and compiles once per realm on first run, since
a compiled function belongs to one realm.

## Phases

1. Realm struct, field swap, GC roots, split init; create a realm and
   evaluate code in it with no cross-realm calls. (done)
2. Function realms, call switching, interpreter and JIT guards, compile
   under the function's realm, coroutines. (done)
3. Per-realm protectors and cross-realm invalidation. (done)
4. `node:vm`: `Script`, `runInThisContext`, `runInNewContext`,
   `createContext` / `runInContext` (contextified globals backed by the
   context object), `DONT_CONTEXTIFY`, `isContext`, `compileFunction`.
   (done)

## Status

Phase 1 done:

- `include/realm.h`, `src/realm.c`: `ant_realm_t`, the per-realm field lists
  (`ANT_REALM_FIELDS`, `ANT_REALM_SYM_FIELDS`, and `ANT_REALM_BUILTIN` /
  `ANT_REALM_MUTABLE_ROOT` entries in `isolate_values.h`), save/load on
  `ant_realm_enter`, GC marking of inactive realms from `gc_mark_roots`
  (minor and major).
- `isolate_init` builds the main realm through `realm_init_core`; a new
  realm runs `realm_init_core` and `ant_bootstrap_realm_intrinsics` (the
  ECMAScript `init_*` modules; `init_symbol_ctor` split from the agent-wide
  well-known symbols) inside itself.
- `this_val` is per-call state, not realm state: natives set it to their
  receiver, so it is not swapped.
- `ant:internal/realm` (`create`, `evaluate`) is the test vehicle until
  `node:vm` exists; `evaluate` uses repl compilation so a realm keeps its
  top-level lexicals and returns completion values.
- Validation: `tests/test_realm_basics.cjs` (also under `ANT_GC_STRESS`),
  harness, spec suite. Against master: instructions within 0.1%, startup
  +40k instructions.

Phase 2 done:

- `sv_func_t.realm` is set when the compiler allocates the function, so
  closures, bound functions (which share `func`), generators and async
  functions all carry it.
- C entry switches the realm: `sv_execute_entry_common`,
  `sv_resume_suspended` (a continuation's frames share one realm, since
  inline calls stay within one), `sv_call_resolve_closure` for generator and
  async calls (the generator object or promise belongs to the callee's
  realm), JIT compilation, and lazy function-object creation.
- `sv_jit_invoke` costs one compare: a callee of another realm goes through
  the cold `sv_jit_invoke_in_realm`. `sv_callback_prepare` sends another
  realm's callback to the generic call, so per-element callback calls use the
  unchecked `sv_jit_invoke_same_realm`.
- The interpreter's inline call paths (`CALL`, `CALL_METHOD`, both tail
  calls) take only same-realm callees.
- Compiled code is guarded only once a second realm exists, so programs with
  one realm compile exactly as before. Code compiled after that gets caller
  guards on direct calls, `new`, `apply` forwarding and devirtualized calls
  (`mir_emit_realm_guard`), plus a prologue check that re-enters through
  `jit_helper_realm_reenter` when entered from another realm. The prologue
  covers code compiled before the realm existed, which has no guards (for
  example a top-level loop entered by OSR that creates a realm midway).
  Inlining requires the callee's realm to match.
- Native function objects (`js_obj_to_func_ex`) get an explicit
  `Function.prototype` from the realm that creates them, so another realm sees
  their own; they used to resolve lazily against the active realm.
- Validation: `tests/test_realm_calls.cjs` (also under `ANT_GC_STRESS`),
  harness, spec suite. Against master: call benchmarks within 0.1% or better,
  bench-v8 -0.1% instructions, ytdlp flat.

Builtins:

- A builtin value points at an interned registry entry whose record starts
  with the meta and the builtin's realm (`ant_cfunc_record_t`; NULL is the
  main realm). While a realm initializes, `ant_cfunc_intern_realm` makes
  interning return that realm's own copies, so builtins differ between
  realms (and so do their promoted objects and name-exposed copies).
- `sv_invoke_builtin` runs a builtin in its realm: one compare against
  `js->builtin_realm` (NULL while the main realm is active), with the switch
  in the cold `sv_invoke_builtin_in_realm`. Indirect `eval` and `Function`
  therefore compile in their own realm. The JIT's builtin fast call always
  compares the builtin's realm with the compiling function's (a load and a
  branch), because code compiled before a second realm existed can be handed
  that realm's builtins later and C builtins have no prologue to catch it.
  This costs +0.15% instructions on a builtin-call loop and moves cycles
  by about 1-2% either way through layout.
- Builtins made at runtime get the active realm too: realm activation keeps
  `ant_cfunc_intern_realm` (for `js_mkfun` interning) in step with
  `js->builtin_realm`, and `js_mkfun_dyn(js, fn)` (promise resolvers,
  `js_heavy_mkfun`, Date and Temporal methods) looks records up by
  `(fn, realm)`. Before, these were main-realm builtins in every realm, so a
  realm's promise resolvers ran in the main realm and `Date.prototype`
  methods were shared between realms. Registry lookups no longer take the
  lock (entries are never freed and only prepended; a miss rechecks and
  inserts under it), which more than pays for the realm checks on async
  code: -0.8% instructions against master on an await loop.
- The builtin-valued `js->sym` fields (`promise_then`, `array_push_fn`,
  `math_fns`, `eval_fn`, ...) are per realm; `js_builtin_eval` returns the
  active realm's, which the compiler uses to recognise direct eval.
- A builtin's prototype is its realm's `Function.prototype`
  (`Object.getPrototypeOf(parseInt)` used to be `null`), promotion runs in the
  builtin's realm, and builtins are valid left operands of `instanceof`.
- `ArraySpeciesCreate` treats another realm's `%Array%` as no constructor.

Globals of a new realm match a Node vm context: `Buffer`, `atob`, `btoa`,
`AsyncIterator`, `InternalError` and `import` are removed and `console` has
methods that do nothing (`ant_realm_install_console`).

Natives find prototypes in per-realm `builtins` tables filled at init
instead of looking up the constructor's name on the global, which was
user-writable and belonged to the wrong realm (`js_get_ctor_proto` is gone):
`typedarray_protos[TypedArrayType]`, `error_protos[js_err_type_t]`,
`arraybuffer_proto`, `sharedarraybuffer_proto`, `dataview_proto`,
`regexp_proto`, `date_proto`, `map_proto`, `set_proto`, `weakmap_proto`,
`weakset_proto`, `weakref_proto`, `finreg_proto`, `suppressederror_proto`,
`disposable_stack_proto`, `async_disposable_stack_proto`; RegExp species and
`String.prototype.matchAll` use `regexp_ctor_value`. Main-realm web types use
their main-only entries (`buffer_proto`, `event_proto`, `eventtarget_proto`,
`message_event_proto`, `observable_proto`). `js_mkdate` makes Dates for fs,
cron, structured clone and N-API. Typed array creation is 7.6% faster and
Map/Set/RegExp/Date/error creation about 4% fewer instructions;
`SharedArrayBuffer.prototype.slice` now returns a SharedArrayBuffer.

Validation: `tests/test_realm_builtins.cjs`, `tests/test_realm_intrinsics.cjs`
(also under `ANT_GC_STRESS`), harness, spec suite. Against master: bench-v8
-0.25% instructions and -0.9% cycles, ytdlp flat.

Known gaps (the ones below the first were closed after phase 4, see
"Phase 4 gaps closed"):

- Lazy prototypes of other object kinds are unchanged.
- The `vm.Module` family (behind `--experimental-vm-modules` in Node) is not
  implemented.

Phase 3 done:

- `ant_property_mutation_invalidate` and
  `ant_symbol_property_mutation_invalidate` check the active realm inline and,
  with several realms, every inactive one (`ant_realm_protectors_invalidate`,
  `ant_realm_symbol_protectors_invalidate`). Writes while a realm initializes
  (`ant_realm_t.initializing`) touch only its new objects and skip the other
  realms; before this, creating a realm turned off the main realm's array
  species and iteration fast paths, because `Array.prototype` is an array.
- Compiled stores of protector keys read `js->realms->next` at run time and
  call `jit_helper_protector_store` when several realms exist, since the
  inline check compares against the compiling realm's intrinsics. No test
  reaches that inline path with another realm's object today (realm creation
  resets inline caches and realms' prototypes do not share shapes); the
  check keeps it correct without relying on that.
- RegExp state is per realm (`ant_regexp_realm_t js->regexp`): the legacy
  statics' match offsets (only the subject was swapped before, so `RegExp.$1`
  after another realm's match threw "invalid regexp static snapshot") and the
  exec/replace write guards. A realm's own RegExp init arms its guard, so
  creating a realm no longer re-arms the main realm's after `exec` was
  patched; a write after init clears every armed realm's guard, since the
  written object's realm is unknown.
- Validation: `tests/test_realm_protectors.cjs` (each case fails on the
  phase 2 build except the main-realm exec one, which a later write happened
  to cover), also under `ANT_GC_STRESS`; harness; spec suite. Against master:
  bench-v8 -0.24% instructions and -0.3% cycles, ytdlp +0.1% instructions
  with flat cycles, the call benchmarks within noise.

Phase 4 done:

- `src/modules/vm.c` is `node:vm`: `Script` (parsed when made, compiled
  once per realm on first run, `filename`/`lineOffset`/`columnOffset`,
  `sourceMapURL`; code caches are accepted and produced empty),
  `runInThisContext` / `runInContext` / `runInNewContext`,
  `createContext` (contextify or `DONT_CONTEXTIFY`), `isContext`,
  `compileFunction` (`params`, `parsingContext`, `filename`),
  `measureMemory`, `constants`.
- Scripts compile with `js_vm_script_compile` (script grammar, repl
  declarations, `<script>` frames) and run with `js_vm_script_run`
  (`this` = `js->global_this`).
- A contextified realm's global is flagged exotic, so the inline global
  store, global ICs and JIT snapshots refuse it and every access takes the
  slow paths (`sv_global_put_slow`, `sv_global_get_interned_miss`,
  `sv_global_undefined_check`, `sv_sandbox_global_delete`), which consult
  the sandbox out of line; the common paths are unchanged.
- The global proxy's traps live in `vm.c`; `preventExtensions` throws
  "Cannot freeze" as in Node.
- `microtaskMode: 'afterEvaluate'` gives the realm its own job queue:
  `js->microtask_realm` (a realm field, NULL normally) routes promise jobs
  queued while the realm is active, and `runInContext` runs them after the
  script (`ant_realm_run_microtasks`). `nextTick` stays global.
- Found while making contexts in a loop, both since builtins got realms:
  the builtin promotion cache's `uint8_t` capacity wrapped from 128 to 0 and
  read freed memory (crash after about 150 realms), and every realm's builtin
  records shared the registry's fixed bucket chains, so creating realms was
  quadratic (1,000 took 17 s). The promotion and name caches are now
  `ant_ptr_pair_map_t` hash maps, a realm's builtin records live in its own
  map (`ant_realm_t.builtin_records`), and the main realm's in the shared
  registry; 1,000 realms take 0.3 s.
- Classes without `extends` take `%Object.prototype%` instead of looking up
  the global `Object` by name.
- Validation: `tests/test_vm.cjs` (documentation examples, globals of a
  contextified realm, compiled code reading a changing context object, a
  proxy context, `compileFunction`, script errors and offsets, microtasks,
  300 contexts), also under `ANT_GC_STRESS`; harness; spec suite.
- Interpreter instruction counts move between two states with unrelated
  changes (LTO code generation of `sv_execute_frame`): `--jitless`
  arithmetic loops ran 6% below master from the builtins work through
  phase 3 and are back at master's count now. A separate investigation.

Phase 4 gaps closed:

- `with` scopes are compile-time scopes: the object lives in a hidden local
  (`"\x01with<scope>"`), and every identifier a `with` may shadow compiles to
  `OP_WITH_TRY_GET/CALL/PUT/DEL` per enclosing `with` (innermost first, found
  through upvalues across functions), each jumping past the plain access
  when the object has the binding and `@@unscopables` allows it. Closures
  made inside a `with` capture the object like any local, so
  `with (o) { f = () => x }` reads `o.x` (it read the global before); the
  frame's single `with_obj` is gone. Code without `with` compiles as before.
- `compileFunction` `contextExtensions` wraps the function in one `with`
  per extension (params and body are checked by a plain compile first).
- `codeGeneration` is a realm field (`codegen_blocked`): `eval`, the
  `Function` family and inlined literal `eval` throw EvalError, and wasm
  compilation throws, when the bit is set. `name`/`origin` are validated.
- `importModuleDynamically` is stored on the script's function tree
  (`sv_func_debug_t.import_handler`/`import_referrer`, marked with the
  function): `OP_IMPORT` with a handler calls it, the main-context symbol
  uses the main loader, and none rejects with
  `ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING`.
- `timeout`/`breakOnSigint`: a watchdog thread or the SIGINT handler sets
  `js->interrupt_pending`. The interpreter reads it on the GC tick's slow
  path (every 1,024 back-edges) and in inline tail calls, so the hot path
  is unchanged. Compiled code polls only once a run has armed interrupts
  (`js->interrupts_armed`; arming drops existing JIT code). The thrown error
  carries `SLOT_VM_TERMINATION`, `js->terminating` skips handlers until the
  run's boundary converts it to `ERR_SCRIPT_EXECUTION_TIMEOUT` or
  `ERR_SCRIPT_EXECUTION_INTERRUPTED`, and the rejection and uncaught
  reporters ignore it.
- Realms are collected. Major GCs mark only the main and active realms as
  roots; a realm is reached through a live closure (`func->realm`) or a live
  builtin (its record's realm), and a context object holds a builtin made
  in its realm (`SLOT_VM_CONTEXT`). `ant_realm_collect` frees the unreached
  ones (not those with queued `afterEvaluate` jobs) before the sweep, with
  their lexicals, maps, iterator registry (now a realm field; the shared
  one pinned every realm) and per-realm script cache (`vm_scripts`). Their
  builtin records go to a pool reused only for an identical meta (same fn,
  name, length and flags), since compiled code may compare a record's
  address. Making 20,000 contexts peaks at 237 MB (3.3 GB before, and
  5,000 at 193 MB against 945 MB).
- Validation: `tests/test_with_scopes.cjs`, `tests/test_vm.cjs` (extensions,
  codeGeneration, importModuleDynamically, timeout, SIGINT),
  `tests/test_vm_realm_gc.cjs`, each also under `ANT_GC_STRESS`; harness;
  spec suite. Against HEAD (no-PGO): bench-v8 flat (cycles +0.06%,
  instructions -0.06%); `--jitless` arithmetic, call, global and sloppy
  loops 4-7% fewer instructions and up to 3% fewer cycles.
- Interpreter regression found and fixed: `--jitless` class-method loops
  ran 8-9% more cycles than HEAD with fewer instructions. CPU counters
  showed 4-5x more memory-order violations (`ST_MEM_ORDER_VIOL_LD_NONSPEC`),
  at the `vm->sp` stores ending put-field and the stack-push handlers. The
  cause was `sv_err`: `VM_CHECK` writes every op's result to it and
  `OP_CATCH` read it, so it stayed live across the whole dispatch loop. In
  the register allocation the codeGeneration check happened to produce, it
  lived in a stack slot written before most dispatches. `OP_CATCH` now
  takes the caught value from the frame's completion (`sv_vm_throw` records
  it there and pushes it), so `sv_err` dies after each check. Interpreter
  loops now run 2-11% fewer instructions than HEAD.
- Class methods (`HAS_SUPER` from their home object) now take the inline
  `CALL`/`CALL_METHOD` path with `frame->super_val` set from the closure,
  instead of a C call through `sv_vm_call`. Against HEAD: `--jitless`
  method loops 16-20% fewer cycles, the JIT `c_method` loop 11%.
- Remaining: splay runs 0.5% more instructions in GC, added gradually across
  the phases (none of the GC code paths changed enough to explain it alone);
  bench-v8 overall is flat.

Review fixes after the merge into master:

- `var` and Annex B function declarations inside `with` are hoisted
  (`collect_var_names`, `hoist_var_decls`, `annex_b_collect_block_var_funcs`
  had no `N_WITH` case).
- Compiler paths that write or type a local directly check
  `local_with_shadowed`: `+=` through `ADD_LOCAL`, the for-update
  `INC_LOCAL`/`DEC_LOCAL`, `var` initializers, `for (var k in ...)` targets
  and `infer_expr_type`.
- `ant_realm_main` reads `js->main_realm`.
- A realm is a minor-GC root only while `gc_dirty`: set when its fields are
  saved or its promoted-builtin or script maps gain an entry, cleared when it
  is marked. Every minor GC promotes its survivors, so a clean realm holds no
  young values.
- `WITH_TRY_*` look the name up once: a shape walk over ordinary objects
  (interned atom, data properties, no `length`), the unscopables cache, and
  the value read from the same lookup. Exotic objects, proxies, accessors and
  `length` take the generic `in`/get path. Stores use `setprop_interned`.

## Risks

- Protector invalidation for inactive realms: silent wrong results if a
  path is missed.
- Values baked at compile time (global snapshot, inlined sloppy `this`,
  promise protector pointers) are correct only while compilation runs in
  the function's realm.
