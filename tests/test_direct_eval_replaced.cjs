const assert = require('node:assert');

function child() {
  const vm = require('node:vm');
  const out = [];
  const realEval = eval;
  function plain(x) { return eval('x + 1'); }
  function plainDyn(x, s) { return eval(s); }
  function plainNone() { return eval(); }
  function plainNonString(x) { return eval({ v: x }).v; }
  function plainSpread(x) { return eval(...['x + 1']); }
  function plainExtra(x) { return eval('x', 'second'); }
  function all(x) { return [plain(x), plainDyn(x, 'x * 2'), plainNone(), plainNonString(x), plainSpread(x), plainExtra(x)].map(String).join(','); }
  for (let i = 0; i < 3000; i++) all(i);
  out.push(all(1));

  globalThis.eval = function (...a) { return 'R(' + a.map(String).join('/') + ')'; };
  out.push(all(1));
  globalThis.eval = realEval;
  out.push(all(2));

  eval = (s) => 'assigned:' + s;
  out.push(plain(3));
  eval = realEval;
  out.push(plain(4));

  Object.defineProperty(globalThis, 'eval', { get() { return (s) => 'getter:' + s; }, configurable: true });
  out.push(plain(5));
  Object.defineProperty(globalThis, 'eval', { value: realEval, writable: true, configurable: true });
  out.push(plain(6));

  function loop() {
    const r = [];
    for (let i = 0; i < 5000; i++) {
      if (i === 4000) globalThis.eval = (s) => 'loop:' + s;
      r.push(eval('i'));
    }
    globalThis.eval = realEval;
    return r[3999] + '|' + r[4000];
  }
  out.push(loop());

  delete globalThis.eval;
  try { out.push(plain(7)); } catch (e) { out.push(e.constructor.name); }
  globalThis.eval = realEval;
  out.push(plain(8));

  out.push(vm.runInNewContext('globalThis.eval = (s) => "ctx:" + s; function f(x) { return eval("x + 1"); } f(1) + "/" + eval("2")'));
  out.push(plain(9));

  const f = new Function('x', 'return eval("x + 1")');
  for (let i = 0; i < 3000; i++) f(i);
  out.push(f(10));
  vm.runInThisContext('let eval = (s) => "lexical:" + s;');
  out.push(f(11));

  console.log(out.join('\n'));
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
    timeout: 120000,
  });
  const expected = [
    '2,2,undefined,1,2,1',
    'R(x + 1),R(x * 2),R(),undefined,R(x + 1),R(x/second)',
    '3,4,undefined,2,3,2',
    'assigned:x + 1',
    '5',
    'getter:x + 1',
    '7',
    '3999|loop:i',
    'ReferenceError',
    '9',
    'ctx:x + 1/ctx:2',
    '10',
    '11',
    'lexical:x + 1',
  ].join('\n');
  for (const [name, args, debug] of [['jitless', ['--jitless'], ''], ['default', [], ''], ['force', [], 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug }, args);
    assert.strictEqual(child.status, 0, `${name}: ${child.stdout}${child.stderr.slice(-2000)}`);
    assert.strictEqual(child.stdout.trim(), expected, `${name}: replaced global eval`);
  }
  console.log('ok');
}
