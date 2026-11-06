const assert = require('node:assert');

function child() {
  const out = [];
  function basic(x) { return eval(...['x + 5']); }
  function empty(x) { return String(eval(...[])); }
  function extra(x) { return eval(...['x * 2', 'ignored']); }
  function mixed(x) { return eval('x', ...[]) + eval(...['x'], 1); }
  function nonString(x) { return eval(...[{ v: x }]).v; }
  function declares(x) { eval(...['var leaked = x + 100']); return leaked; }
  function strictDecl(x) { 'use strict'; eval(...['var hidden = 1']); return typeof hidden; }
  function arrowThis() { return (() => eval(...['this.v']))(); }
  function nested(x) { eval('var y = x + 1'); return eval(...['y + 1']); }
  function order() {
    const log = [];
    const it = { *[Symbol.iterator]() { log.push('iter'); yield 'log.length'; } };
    log.push('before');
    const r = eval(...it);
    return log.join(',') + '=' + r;
  }
  function throwsSyntax() { try { eval(...['(']); return 'no'; } catch (e) { return e.constructor.name; } }
  for (let i = 0; i < 300; i++) { basic(i); extra(i); nested(i); }
  out.push(basic(1), empty(2), extra(3), mixed(4), nonString(5), declares(6), strictDecl(7), arrowThis.call({ v: 8 }), nested(9), order(), throwsSyntax());

  const realEval = globalThis.eval;
  function viaGlobal(x) { eval('var z = x'); return eval(...['typeof x']); }
  globalThis.eval = (s) => 'replaced:' + s;
  out.push(viaGlobal(10));
  globalThis.eval = realEval;
  out.push(viaGlobal(11));
  console.log(out.join(' | '));
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
  const expected = '6 | undefined | 6 | 8 | 5 | 106 | undefined | 8 | 11 | before,iter=2 | SyntaxError | replaced:typeof x | number';
  for (const [name, args, debug] of [['jitless', ['--jitless'], ''], ['default', [], ''], ['force', [], 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug }, args);
    assert.strictEqual(child.status, 0, `${name}: ${child.stdout}${child.stderr.slice(-2000)}`);
    assert.strictEqual(child.stdout.trim(), expected, `${name}: spread direct eval`);
  }
  console.log('ok');
}
