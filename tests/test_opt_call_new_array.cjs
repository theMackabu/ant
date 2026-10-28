const assert = require('node:assert');

const WARM = 3000;

function warm(name, fn, expected) {
  for (let i = 0; i < WARM; i++) assert.deepStrictEqual(fn(), expected, `${name}: warm-up`);
  console.log(name, JSON.stringify(expected));
}

function child() {
  function makeList() { return new Array(); }
  warm('new array', () => {
    const list = makeList();
    list.push(1, 2);
    return [Array.isArray(list), Object.getPrototypeOf(list) === Array.prototype, list.length];
  }, [true, true, 2]);

  function shadowed() { const Array = function () { this.shadow = true; }; return new Array(); }
  warm('shadowed array', () => shadowed().shadow, true);

  const RealArray = Array;
  function viaGlobal() { return new Array(); }
  function driveArray(n) { let last; for (let i = 0; i < n; i++) last = viaGlobal(); return last; }
  for (let i = 0; i < 200; i++) assert.strictEqual(Array.isArray(driveArray(50)), true);
  globalThis.Array = function () { this.replaced = true; };
  const replacedArray = driveArray(3);
  globalThis.Array = RealArray;
  assert.strictEqual(replacedArray.replaced, true);
  assert.strictEqual(Array.isArray(driveArray(3)), true);
  console.log('array replaced', true);

  function sloppyThis() { return this; }
  function strictThis() { 'use strict'; return this; }
  function callWith(f, receiver) { return f.call(receiver); }
  warm('sloppy undefined', () => callWith(sloppyThis, undefined) === globalThis, true);
  warm('sloppy null', () => callWith(sloppyThis, null) === globalThis, true);
  warm('sloppy primitive', () => typeof callWith(sloppyThis, 5), 'object');
  warm('strict undefined', () => callWith(strictThis, undefined), undefined);
  warm('strict primitive', () => callWith(strictThis, 5), 5);

  function noArgs(f) { return f.call(); }
  warm('no args', () => noArgs(strictThis), undefined);

  function Base(x) { this.x = x; }
  function Derived(x, y) { Base.call(this, x); this.y = y; }
  warm('super call', () => { const d = new Derived(1, 2); return [d.x, d.y]; }, [1, 2]);

  function countArgs() { return arguments.length + ':' + Array.prototype.slice.call(arguments).join(); }
  function forward(a, b, c) { return countArgs.call(null, a, b, c); }
  warm('arguments', () => forward(1, 2, 3), '3:1,2,3');

  const bound = function () { return this.tag; }.bind({ tag: 'bound' });
  function callBound(f) { return f.call({ tag: 'ignored' }); }
  warm('bound', () => callBound(bound), 'bound');

  function tag(x) { return Object.prototype.toString.call(x); }
  warm('native callee', () => [tag([]), tag(null), tag(1)], ['[object Array]', '[object Null]', '[object Number]']);

  function viaCall(f, receiver) { return f.call(receiver, 1); }
  const method = function (v) { return this.base + v; };
  function driveCall(n) { let last; for (let i = 0; i < n; i++) last = viaCall(method, { base: 10 }); return last; }
  for (let i = 0; i < 200; i++) assert.strictEqual(driveCall(50), 11);
  const realCall = Function.prototype.call;
  Function.prototype.call = function () { return 'replaced'; };
  const replaced = driveCall(3);
  Function.prototype.call = realCall;
  assert.strictEqual(replaced, 'replaced');
  assert.strictEqual(driveCall(3), 11);
  console.log('call replaced', replaced);

  function callNonCallable(target) { return Function.prototype.call.call(target); }
  warm('callable target', () => callNonCallable(strictThis), undefined);
  assert.throws(() => callNonCallable(5), TypeError);
  assert.throws(() => callNonCallable({}), TypeError);
  console.log('non-callable', 'TypeError');
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.match(interp.stdout, /non-callable "?TypeError"?\n$/);
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    console.log(`PASS opt call and new Array ${name}`);
  }
}
