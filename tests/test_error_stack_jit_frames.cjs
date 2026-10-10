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

console.log('compiled frames match the interpreter');
