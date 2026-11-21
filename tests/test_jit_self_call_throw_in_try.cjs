const assert = require('node:assert');

function child() {
  const und = undefined;
  const out = [];

  function rethrown(d) {
    let h = 3;
    try {
      if (d < 1) h = h + rethrown(d + 1);
      else throw -22;
    } finally { }
    return h;
  }
  function caught(d) {
    let x = d * 1.5;
    try {
      if (d < 1) return x + caught(d + 1);
      throw -22;
    } catch (e) {
      if (d < 1) return typeof e + ':' + e;
      throw e;
    }
  }
  function caughtObject(d) {
    const pad = [d, d + 1];
    try {
      if (d < 1) return pad[0] + caughtObject(d + 1);
      throw { tag: 7 };
    } catch (e) {
      if (d < 1) return e && e.tag;
      throw e;
    }
  }
  function deepStack(d) {
    let a = 1, b = 'b', c = 2.5;
    try {
      if (d < 1) return a + (b + (c + deepStack(d + 1)));
      throw 'deep';
    } catch (e) {
      if (d < 1) return e;
      throw e;
    }
  }
  function nested(d) {
    let h = 5;
    try {
      try {
        if (d < 1) h = h * nested(d + 1);
        else throw 41;
      } finally { h += 1; }
    } catch (e) {
      if (d < 1) return 'outer ' + e + ' ' + h;
      throw e + 1;
    }
    return h;
  }
  function inCatch(d) {
    let h = 9;
    try { throw 1; } catch (first) {
      try {
        if (d < 1) h = h + inCatch(d + 1);
        else throw 77;
      } catch (e) {
        if (d < 1) return first + ':' + e;
        throw e;
      }
    }
    return h;
  }
  function inFinally(d) {
    let h = 9;
    try { h += 1; } finally {
      try {
        if (d < 1) h = h + inFinally(d + 1);
        else throw 78;
      } catch (e) {
        if (d < 1) return 'finally ' + e;
        throw e;
      }
    }
    return h;
  }
  function captured(d) {
    let h = 4, seen = 'none';
    const read = () => seen;
    try {
      if (d < 1) h = h + captured(d + 1);
      else throw 55;
    } catch (e) {
      seen = e;
      if (d < 1) return read();
      throw e;
    }
    return h;
  }
  function generic(d, i) {
    let h = 3;
    if ((i & 3) === 1) und.x;
    try {
      if (d < 1) h = h + generic(d + 1, i);
      else throw -22;
    } finally { }
    return h;
  }

  const fns = [rethrown, caught, caughtObject, deepStack, nested, inCatch, inFinally, captured, generic];
  for (const fn of fns) {
    const seen = [];
    for (let i = 0; i < 600; i++) {
      let r;
      try { r = 'returned ' + fn(0, i); } catch (e) { r = 'threw ' + (e instanceof TypeError ? 'TypeError' : e); }
      if (!seen.includes(r)) seen.push(r);
    }
    out.push(fn.name + ': ' + seen.join(' | '));
  }
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
    'rethrown: threw -22',
    'caught: returned number:-22',
    'caughtObject: returned 7',
    'deepStack: returned deep',
    'nested: returned outer 42 6',
    'inCatch: returned 1:77',
    'inFinally: returned finally 78',
    'captured: returned 55',
    'generic: threw -22 | threw TypeError',
  ].join('\n') + '\n';
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.strictEqual(interp.stdout, expected);
  for (const [name, debug] of [['default', ''], ['force', 'opt:force'], ['verify', 'opt:verify'], ['off', 'opt:off']]) {
    const result = run({ ANT_DEBUG: debug });
    assert.strictEqual(result.status, 0, `${name}: ${result.error || result.stdout + result.stderr.slice(-2000)}`);
    assert.strictEqual(result.stdout, expected, `${name}: output differs`);
  }
  console.log('ok');
}
