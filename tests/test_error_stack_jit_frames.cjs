// Stacks captured while compiled code is running must match the interpreter's:
// compiled frames, inlined callees and OSR-entered loops are found on the
// native stack (src/jit/unwind.c), not in VM frames.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const cases = {
  chain: `
    function c(i) { const e = new Error('c' + i); return e; }
    function b(i) { const r = c(i); return r; }
    function a(i) { const r = b(i); return r; }
    let last; for (let i = 0; i < 20000; i++) last = a(i);
    console.log(last.stack);
  `,
  osrTopLevel: `
    let k = 0; for (let i = 0; i < 100000; i++) k += i;
    console.log(new Error('after loop').stack);
  `,
  osrInFunction: `
    function inner(n) { return n < 0 ? null.z : n; }
    function hot() {
      let s = 0;
      for (let i = 0; i < 100000; i++) s += inner(i);
      try { inner(-1); } catch (e) { console.log(e.stack); }
      return s;
    }
    hot();
  `,
  nativeCallback: `
    function leaf(i) { if (i === 19999) throw new TypeError('leaf'); return i; }
    function viaMap(i) { return [i].map(leaf)[0]; }
    function outer(i) { try { return viaMap(i); } catch (e) { return e; } }
    let last; for (let i = 0; i < 20000; i++) last = outer(i);
    console.log(last.stack);
  `,
  bailout: `
    function g(x) { return x * 2; }
    function f(x) { const v = g(x); if (typeof x === 'string') throw new Error('bail ' + v); return v; }
    for (let i = 0; i < 20000; i++) f(i);
    try { f('s'); } catch (e) { console.log(e.stack); }
  `,
  resumedActivation: `
    function g(x) { const n = x * 2; const s = new Error('g').stack; return typeof x === 'number' ? n : s; }
    function h(x) { const r = g(x); return r; }
    for (let i = 0; i < 20000; i++) h(i);
    console.log(h('s'));
    function k(x) { let t = 0; for (let i = 0; i < 3; i++) t += x[i]; const s = new Error('k').stack; return typeof t === 'number' ? t : s; }
    function m(x) { const r = k(x); return r; }
    for (let i = 0; i < 20000; i++) m([1, 2, 3]);
    console.log(m(['a', 'b', 'c']));
  `,
  getterName: `
    const getter = { get v() { return new Error('getter').stack; } };
    function readIt(o) { const s = o.v; return s; }
    let s; for (let i = 0; i < 20000; i++) s = readIt(getter);
    console.log(s);
  `,
  callsites: `
    Error.prepareStackTrace = (e, cs) => cs.map(c => c.getFunctionName() + ':' + c.getLineNumber()).join(',');
    function deep(i) { const o = {}; Error.captureStackTrace(o); return o.stack; }
    function mid(i) { const s = deep(i); return s; }
    let s; for (let i = 0; i < 20000; i++) s = mid(i);
    console.log(s);
  `,
  callerColumn: `
    function g(x) { return new Error('g' + x); }
    function f(x) { const s = g(x).stack; return s.split('\\n')[2]; }
    let s; for (let i = 0; i < 20000; i++) s = f(i);
    console.log(s);
    let k = 0; for (let i = 0; i < 100000; i++) k += i;
    console.log(f('s'));
  `,
  computedNames: `
    class K { ['dyn' + 'Name']() { return new Error('k').stack.split('\\n')[1]; } }
    const k = new K();
    const o = { get ['v' + 1]() { return new Error('o').stack.split('\\n')[1]; } };
    function rd(o) { return o.v1; }
    let a, b; for (let i = 0; i < 20000; i++) { a = k.dynName(); b = rd(o); }
    console.log(a); console.log(b);
  `,
  limitThroughInlined: `
    Error.stackTraceLimit = 3;
    function leaf(x) { return new Error('l' + x); }
    function mid(x) { return leaf(x) || 0; }
    function top(x) { const e = mid(x); return e; }
    function outer(x) { const e = top(x); return e; }
    let s; for (let i = 0; i < 20000; i++) s = outer(i).stack;
    console.log(s);
  `,
  subclassSkip: `
    class Named extends Error { constructor(m) { super(m); } }
    class Sub extends Named {}
    function mk(i) { const e = i & 1 ? new Named('n' + i) : new Sub('s' + i); return e; }
    let a, b; for (let i = 0; i < 20000; i++) { if (i & 1) a = mk(i); else b = mk(i); }
    console.log(a.stack); console.log(b.stack);
  `,
  constructorFrames: `
    class P { constructor(i) { this.e = new Error('p' + i); } }
    class Q extends P { constructor(i) { super(i); } }
    function mk(i) { const q = new Q(i); return q.e; }
    let last; for (let i = 0; i < 20000; i++) last = mk(i);
    console.log(last.stack);
  `,
  inlinedPlainCtor: `
    function helper() { return new Error('h'); }
    function Ctor2() { this.e = helper(); }
    function mkCtor2() { return new Ctor2(); }
    const seen = new Set();
    for (let i = 0; i < 20000; i++) seen.add(mkCtor2().e.stack);
    console.log([...seen].join('\\n--\\n'));
  `,
  resumedCtor: `
    function P2() { let x = 0; for (let i = 0; i < 200000; i++) x += 1; const m = 'osr' + x; this.e = new Error(m); }
    console.log(new P2().e.stack);
  `,
  deepCtorChain: `
    Error.stackTraceLimit = 2;
    class A extends Error { constructor(m) { super(m); this.a = 1; } }
    class B extends A { constructor(m) { super(m); this.b = 1; } }
    class C extends B { constructor(m) { super(m); this.c = 1; } }
    class D extends C { constructor(m) { super(m); this.d = 1; } }
    function mk(i) { const e = new D('d' + i); return e; }
    function f1(i) { const e = mk(i); return e; }
    function f2(i) { const e = f1(i); return e; }
    function f3(i) { const e = f2(i); return e; }
    let last; for (let i = 0; i < 20000; i++) last = f3(i);
    console.log(last.stack);
  `,
  captureAbove: `
    function inner(fn) { const o = {}; Error.captureStackTrace(o, fn); return o.stack; }
    function outer(fn) { const s = inner(fn); return s; }
    function caller(fn) { const s = outer(fn); return s; }
    let s; for (let i = 0; i < 20000; i++) s = caller(outer);
    console.log(s);
  `,
  uncaught: `
    function c(i) { if (i === 19999) throw new Error('boom ' + i); return i; }
    function b(i) { const r = c(i); return r; }
    function a(i) { const r = b(i); return r; }
    for (let i = 0; i < 20000; i++) a(i);
  `,
  traceInCallback: `
    function work(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }
    function cb(trace) { work(10); if (trace) console.trace('t'); }
    for (let i = 0; i < 20000; i++) cb(false);
    setTimeout(cb, 0, true);
  `,
};

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-jit-frames-'));
try {
  for (const [name, source] of Object.entries(cases)) {
    const file = path.join(root, `${name}.cjs`);
    fs.writeFileSync(file, source);
    const run = (args) => spawnSync(process.execPath, [...args, file], { encoding: 'utf8', env: { ...process.env, NO_COLOR: '1' } });
    const plain = (text) => text.replace(/\x1b\[[0-9;]*m/g, '');
    const jit = run([]);
    const interp = run(['--jitless']);
    assert.strictEqual(jit.status, interp.status, `${name}: ${jit.stderr}`);
    assert.strictEqual(jit.stdout, interp.stdout, `${name}: compiled stack differs from the interpreter's`);
    assert.strictEqual(plain(jit.stderr), plain(interp.stderr), `${name}: compiled stderr differs from the interpreter's`);
  }

  // Closures of one function given different names at runtime: a compiled
  // frame has no closure to read the name from, so it must not report one.
  const file = path.join(root, 'factory.cjs');
  fs.writeFileSync(file, `
    function make(n) { return { [n]: function () { return new Error('x').stack.split('\\n')[1]; } }[n]; }
    const alpha = make('alpha'), beta = make('beta');
    let s1, s2; for (let i = 0; i < 20000; i++) { s1 = alpha(); s2 = beta(); }
    console.log(s1.trim().split(' ')[1], s2.trim().split(' ')[1]);
  `);
  const names = spawnSync(process.execPath, [file], { encoding: 'utf8' }).stdout.trim().split(' ');
  assert.ok(['alpha', '<anonymous>'].includes(names[0]) && ['beta', '<anonymous>'].includes(names[1]), names.join(' '));

  // Naming a frame must not run user code while compiling.
  fs.writeFileSync(file, `
    let calls = 0;
    const f = (() => function () { return 1; })();
    Object.defineProperty(f, 'name', { get() { calls++; return 'nm'; }, configurable: true });
    let t = 0; for (let i = 0; i < 50000; i++) t += f();
    console.log(calls);
  `);
  assert.strictEqual(spawnSync(process.execPath, [file], { encoding: 'utf8' }).stdout.trim(), '0');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

{
  function staticKeyError() { return new Error('static key'); }
  function staticKey() { let e; class C { static [(e = staticKeyError(), 'k')] = 1; } return e.stack; }
  const frames = (stack) => stack.split('\n').slice(1, 3).map((line) => line.trim().split(' ')[1]).join(' < ');
  const first = frames(staticKey());
  assert.strictEqual(first, 'staticKeyError < staticKey');
  for (let i = 0; i < 4000; i++) assert.strictEqual(frames(staticKey()), first, `static computed key frames at call ${i}`);
}

console.log('compiled frames match the interpreter');
