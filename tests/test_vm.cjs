const assert = require('node:assert');
const vm = require('node:vm');
const local = (value) => JSON.parse(JSON.stringify(value));

async function main() {
  {
    const context = { x: 2 };
    vm.createContext(context);
    vm.runInContext('x += 40; var y = 17;', context);
    assert.deepStrictEqual([context.x, context.y, typeof y], [42, 17, 'undefined']);

    const animals = { animal: 'cat', count: 2 };
    const script = new vm.Script('count += 1; name = "kitty";');
    vm.createContext(animals);
    for (let i = 0; i < 10; ++i) script.runInContext(animals);
    assert.deepStrictEqual(animals, { animal: 'cat', count: 12, name: 'kitty' });

    globalThis.globalVar = 0;
    const counter = new vm.Script('globalVar += 1', { filename: 'myfile.vm' });
    for (let i = 0; i < 1000; ++i) counter.runInThisContext();
    assert.strictEqual(globalThis.globalVar, 1000);
    delete globalThis.globalVar;

    const setter = new vm.Script('globalVar = "set"');
    const contexts = [{}, {}, {}];
    contexts.forEach((c) => setter.runInNewContext(c));
    assert.deepStrictEqual(contexts, [{ globalVar: 'set' }, { globalVar: 'set' }, { globalVar: 'set' }]);

    let localVar = 'initial value';
    assert.strictEqual(vm.runInThisContext('localVar = "vm";'), 'vm');
    assert.strictEqual(localVar, 'initial value');
    delete globalThis.localVar;
  }

  {
    const context = vm.createContext();
    const inner = vm.runInContext('globalThis', context);
    assert.notStrictEqual(inner, context);
    assert.throws(() => vm.runInContext('Object.freeze(globalThis);', context), (e) => e.message === 'Cannot freeze');
    assert.strictEqual(vm.runInContext('globalThis.foo = 1; foo;', context), 1);
    assert.strictEqual(context.foo, 1);
    inner.bar = 2;
    assert.strictEqual(context.bar, 2);
    assert.strictEqual(vm.runInContext('this', context), inner);
    assert.strictEqual(inner.Array, vm.runInContext('Array', context));
    assert.deepStrictEqual(local(vm.runInContext('Object.keys(globalThis)', context)).sort(), ['bar', 'foo']);
  }

  {
    const context = vm.createContext(vm.constants.DONT_CONTEXTIFY);
    assert.strictEqual(vm.runInContext('globalThis', context), context);
    assert.notStrictEqual(context.Array, Array);
    vm.runInContext('foo = 1;', context);
    assert.strictEqual(context.foo, 1);
    context.bar = 1;
    assert.strictEqual(vm.runInContext('bar;', context), 1);
    Object.freeze(context);
    assert.throws(() => vm.runInContext('baz = 1; baz;', context), (e) => e.name === 'ReferenceError');
    assert.ok(vm.isContext(context));
    const frozen = vm.runInNewContext('Object.freeze(globalThis); globalThis;', vm.constants.DONT_CONTEXTIFY);
    assert.ok(Object.isFrozen(frozen));
  }

  {
    const context = vm.createContext({ a: 1 });
    const inner = vm.runInContext('this', context);
    const sloppy = vm.runInContext('(function () { return this; })', context);
    assert.strictEqual(sloppy(), inner);
    for (let i = 0; i < 3000; i++) sloppy();
    assert.strictEqual(sloppy(), inner);

    assert.strictEqual(vm.runInContext('let q = 5; q', context), 5);
    assert.strictEqual(vm.runInContext('q + 1', context), 6);
    assert.strictEqual(context.q, undefined);
    assert.throws(() => vm.runInContext('let q = 1', context), (e) => e.name === 'SyntaxError');

    Object.defineProperty(context, 'acc', { get() { return 'got'; }, configurable: true });
    assert.strictEqual(vm.runInContext('acc', context), 'got');
    assert.strictEqual(vm.runInContext('delete a; typeof a', context), 'undefined');
    assert.ok(!('a' in context));
    assert.throws(() => vm.runInContext('"use strict"; undeclared = 1', context), (e) => e.name === 'ReferenceError');
    assert.strictEqual(vm.runInContext('typeof nope', context), 'undefined');
    assert.throws(() => vm.runInContext('nope', context), (e) => e.name === 'ReferenceError');

    const error = vm.runInContext('try { null.x } catch (e) { e }', context);
    assert.ok(error instanceof vm.runInContext('TypeError', context));
    assert.ok(!(error instanceof TypeError));

    vm.runInContext('function decl() { return 7; }', context);
    assert.strictEqual(context.decl(), 7);
    assert.strictEqual(vm.runInNewContext('Array', { Array: 1 }), 1);
    assert.strictEqual(vm.runInNewContext('var x = 1; x', {}), 1);
    const keep = { v: 9 };
    vm.runInNewContext('var v;', keep);
    assert.strictEqual(keep.v, 9);
    assert.strictEqual(vm.runInNewContext('console === c', { console, c: console }), true);
    assert.strictEqual(vm.runInNewContext('typeof setTimeout + typeof require + typeof Buffer'), 'undefinedundefinedundefined');
  }

  {
    const context = vm.createContext({ step: 1 });
    const sum = vm.runInContext('(function () { let s = 0; for (let i = 0; i < 2000; i++) s += step; return s; })', context);
    assert.strictEqual(sum(), 2000);
    context.step = 2;
    for (let i = 0; i < 50; i++) sum();
    assert.strictEqual(sum(), 4000);
    delete context.step;
    assert.throws(() => sum(), (e) => e.name === 'ReferenceError');
  }

  {
    const seen = [];
    const context = vm.createContext(new Proxy({ z: 3 }, {
      get(target, key, receiver) { seen.push(key); return Reflect.get(target, key, receiver); },
    }));
    assert.strictEqual(vm.runInContext('z', context), 3);
    assert.ok(seen.includes('z'));
  }

  {
    assert.strictEqual(vm.compileFunction('return a + b', ['a', 'b'])(2, 3), 5);
    const context = vm.createContext({ k: 5 });
    const fn = vm.compileFunction('return k', [], { parsingContext: context });
    assert.strictEqual(fn(), 5);
    assert.ok(fn instanceof vm.runInContext('Function', context));
  }

  {
    const fn = vm.compileFunction('return a + b + c', ['a'], { contextExtensions: [{ b: 1, c: 100 }, { c: 10 }] });
    assert.strictEqual(fn(5), 16);
    const context = vm.createContext({ g: 'ctx' });
    assert.strictEqual(vm.compileFunction('return g + ext', [], { parsingContext: context, contextExtensions: [{ ext: '!' }] })(), 'ctx!');
    assert.throws(() => vm.compileFunction('}); (function(){', [], { contextExtensions: [{}] }), SyntaxError);
    assert.throws(() => vm.compileFunction('1', [], { contextExtensions: [1] }), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    const counter = { count: 0 };
    const inc = vm.compileFunction('count++; return count', [], { contextExtensions: [counter] });
    inc();
    assert.strictEqual(inc(), 2);
  }

  {
    const script = new vm.Script('function myFunc() {}\n//# sourceMappingURL=sourcemap.json\n');
    assert.strictEqual(script.sourceMapURL, 'sourcemap.json');
    assert.ok(script instanceof vm.Script);
    assert.ok(Buffer.isBuffer(script.createCachedData()));
    assert.strictEqual(new vm.Script('1', { cachedData: Buffer.alloc(0) }).cachedDataRejected, false);
    assert.throws(() => vm.Script('1'), TypeError);
    assert.strictEqual(new vm.Script('[1, 2]').runInNewContext() instanceof Array, false);

    assert.throws(() => new vm.Script('let let'), SyntaxError);
    assert.throws(() => new vm.Script('await 1'), SyntaxError);
    assert.throws(() => new vm.Script(1), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    assert.throws(() => vm.runInContext('1', {}), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    assert.throws(() => vm.isContext(1), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    assert.throws(() => vm.createContext({}, { microtaskMode: 'x' }), (e) => e.code === 'ERR_INVALID_ARG_VALUE');

    try {
      new vm.Script('\n\nthrow new Error("x")', { filename: 'file.vm', lineOffset: 10 }).runInThisContext();
      assert.fail('should throw');
    } catch (e) {
      assert.match(e.stack.split('\n')[1], /file\.vm:13:/);
    }
  }

  {
    const result = vm.runInNewContext('(async () => { await null; return Promise.resolve(41).then((x) => x + 1); })()');
    assert.strictEqual(await result, 42);

    const log = [];
    vm.runInNewContext('Promise.resolve().then(() => log.push("job"))', { log }, { microtaskMode: 'afterEvaluate' });
    assert.deepStrictEqual(log, ['job']);
  }

  {
    const strict = vm.createContext({}, { codeGeneration: { strings: false } });
    for (const src of ['eval("1")', '(0, eval)("1")', '(function () { return eval("1"); })()', 'new Function("return 1")', 'Function("return 1")', '(async function () {}).constructor("return 1")', '(function* () {}).constructor("")'])
      assert.throws(() => vm.runInContext(src, strict), (e) => e.name === 'EvalError' && /disallowed/.test(e.message), src);
    assert.strictEqual(vm.runInContext('1 + 1', strict), 2);
    assert.strictEqual(vm.runInContext('eval(5)', strict), 5);
    assert.strictEqual(vm.compileFunction('return 3', [], { parsingContext: strict })(), 3);
    assert.strictEqual(vm.runInNewContext('eval("2")', {}, { contextCodeGeneration: { strings: true } }), 2);
    assert.throws(() => vm.runInNewContext('eval("2")', {}, { contextCodeGeneration: { strings: false } }), (e) => e.name === 'EvalError');
    assert.strictEqual(eval('3'), 3);

    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    const nowasm = vm.createContext({ bytes }, { codeGeneration: { wasm: false } });
    assert.throws(() => vm.runInContext('new WebAssembly.Module(bytes)', nowasm), (e) => e.name === 'CompileError');
    assert.ok(vm.runInContext('new WebAssembly.Module(bytes)', vm.createContext({ bytes })));
    assert.throws(() => vm.createContext({}, { codeGeneration: { strings: 1 } }), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    assert.throws(() => vm.createContext({}, { name: 1 }), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
    vm.createContext({}, { name: 'ctx', origin: 'https://example.com' });
  }

  {
    await assert.rejects(new vm.Script('import("node:fs")').runInThisContext(), (e) => e.code === 'ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING');
    const fs = await new vm.Script('import("node:fs")', { importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER }).runInNewContext();
    assert.strictEqual(typeof fs.readFileSync, 'function');

    let seen;
    const script = new vm.Script('import("foo.json", { with: { type: "json" } })', {
      async importModuleDynamically(specifier, referrer, attributes) { seen = [specifier, referrer, attributes]; return { bar: 1 }; },
    });
    assert.deepStrictEqual(await script.runInThisContext(), { bar: 1 });
    assert.deepStrictEqual([seen[0], seen[1] === script, seen[2]], ['foo.json', true, { type: 'json' }]);

    const fn = vm.compileFunction('return import("x")', [], { importModuleDynamically: (s, r) => ({ s, same: r === fn }) });
    assert.deepStrictEqual(await fn(), { s: 'x', same: true });
    const late = vm.runInNewContext('(async () => import("y"))', {}, { importModuleDynamically: () => ({ late: true }) });
    assert.deepStrictEqual(await late(), { late: true });
    assert.throws(() => new vm.Script('1', { importModuleDynamically: 1 }), (e) => e.code === 'ERR_INVALID_ARG_TYPE');
  }

  {
    const timedOut = (e) => e.code === 'ERR_SCRIPT_EXECUTION_TIMEOUT' && /timed out after 50ms/.test(e.message);
    assert.throws(() => vm.runInNewContext('while (true) {}', {}, { timeout: 50 }), timedOut);
    assert.throws(() => vm.runInNewContext('for (;;) { try { for (;;) {} } catch (e) {} finally { x = 1 } }', {}, { timeout: 50 }), timedOut);
    assert.throws(() => vm.runInNewContext('"use strict"; function f(n) { if (n < 0) return 0; return f(n); } f(1)', {}, { timeout: 50 }), timedOut);
    function outerLoop() { for (;;) {} }
    assert.throws(() => vm.runInNewContext('loop()', { loop: outerLoop }, { timeout: 50 }), timedOut);
    assert.throws(() => vm.runInThisContext('while (true) {}', { timeout: 50 }), timedOut);
    assert.throws(() => new vm.Script('while (1) {}').runInThisContext({ timeout: 50 }), timedOut);
    assert.throws(() => vm.runInNewContext('(async () => { while (true) {} })()', {}, { timeout: 50 }), timedOut);
    assert.throws(() => vm.runInNewContext('Promise.resolve().then(() => { while (true) {} })', {}, { timeout: 50, microtaskMode: 'afterEvaluate' }), timedOut);
    
    const outer = vm.createContext({ inner: () => vm.runInNewContext('while (true) {}') });
    assert.throws(() => vm.runInContext('inner()', outer, { timeout: 50 }), timedOut);
    const caught = vm.runInNewContext('try { run() } catch (e) { e.code }', { run: () => vm.runInNewContext('for (;;) {}', {}, { timeout: 30 }) });
    assert.strictEqual(caught, 'ERR_SCRIPT_EXECUTION_TIMEOUT');
    
    assert.strictEqual(vm.runInNewContext('let s = 0; for (let i = 0; i < 1e5; i++) s += i; s', {}, { timeout: 5000 }), 4999950000);
    try { throw new Error('caught'); } catch (e) { assert.strictEqual(e.message, 'caught'); }
    assert.throws(() => vm.runInNewContext('1', {}, { timeout: 0 }), (e) => e.code === 'ERR_OUT_OF_RANGE');
    assert.throws(() => vm.runInNewContext('1', {}, { timeout: 'x' }), (e) => e.code === 'ERR_INVALID_ARG_TYPE');

    assert.throws(
      () => vm.runInNewContext('kill(); while (true) {}', { kill: () => process.kill(process.pid, 'SIGINT') }, { breakOnSigint: true }),
      (e) => e.code === 'ERR_SCRIPT_EXECUTION_INTERRUPTED'
    );
  }

  const contextCount = process.env.ANT_GC_STRESS ? 40 : 300;
  for (let i = 0; i < contextCount; i++) {
    const context = vm.createContext({ i });
    assert.strictEqual(vm.runInContext('new Int32Array([i]).subarray(0)[0] + [1].map((x) => x)[0]', context), i + 1);
  }

  const memory = await vm.measureMemory();
  assert.strictEqual(typeof memory.total.jsMemoryEstimate, 'number');
  assert.strictEqual(vm.isContext({}), false);

  console.log('vm: ok');
}

main();
