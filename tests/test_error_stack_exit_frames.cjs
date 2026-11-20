const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const N = 3000;
const mode = process.argv[2];

if (mode) {
  const at = mode === 'early' ? (i) => i === 5 || i === 700 || i === N - 2 : (i) => i === N - 2;
  const out = [];

  const frames = (stack) => {
    const list = [];
    for (const line of String(stack).split('\n').slice(1)) {
      const m = /^\s*at (?:(.*) \()?.*?:(\d+):(\d+)\)?$/.exec(line);
      if (!m) continue;
      const name = (m[1] || '<top>').replace(/^Object\.<anonymous>$|^<function>$/, '<top>').replace(/^K\./, '');
      list.push(`${name}:${m[2]}:${m[3]}`);
      if (name === '<top>') break;
    }
    return list;
  };
  const rec = (name, i, stack) => { if (name !== 'warm') out.push([name, i, frames(stack)]); };

  function leafNew(i) { if (at(i)) return new Error('x').stack; return ''; }
  function leafThrow(i) { if (at(i)) return new Error('x').stack; throw i; }
  function leafLate(i) { if (at(i)) throw new Error('late' + i); return 1; }
  function leafLateType(i, o) { return at(i) ? o.x.y : 1; }
  function leafDeep(i) { const r = leafNew(i); return r; }

  function calleeAfterCatch(n, name) { for (let i = 0; i < n; i++) { try { const r = leafThrow(i); if (r) rec(name, i, r); } catch (e) {} } }
  function calleeAfterFinally(n, name) { for (let i = 0; i < n; i++) { try { try { const r = leafThrow(i); if (r) rec(name, i, r); } finally { i += 0; } } catch (e) {} } }
  function calleeInFinally(n, name) { for (let i = 0; i < n; i++) { try { try { if (i % 3 === 0) throw i; } finally { const r = leafNew(i); if (r) rec(name, i, r); } } catch (e) {} } }
  function calleeNoHandler(n, name) { for (let i = 0; i < n; i++) { const r = leafNew(i); if (r) rec(name, i, r); } }
  function deepInline(n, name) { for (let i = 0; i < n; i++) { try { if (i % 3 === 0) throw i; const r = leafDeep(i); if (r) rec(name, i, r); } catch (e) {} } }
  function lateThrow(n, name) { let s = 0; for (let i = 0; i < n; i++) { try { s += leafLate(i); } catch (e) { rec(name, i, e.stack); } } return s; }
  function lateThrowFinally(n, name) { let s = 0; for (let i = 0; i < n; i++) { try { try { s += leafLate(i); } finally { s++; } } catch (e) { rec(name, i, e.stack); } } return s; }
  function lateThrowAfterCatch(n, name) { let s = 0; for (let i = 0; i < n; i++) { try { if (i % 3 === 0) throw i; s += leafLate(i); } catch (e) { if (typeof e !== 'number') rec(name, i, e.stack); } } return s; }
  function lateType(n, name) { let s = 0; const o = {}; for (let i = 0; i < n; i++) { try { if (i % 3 === 0) throw i; s += leafLateType(i, o); } catch (e) { if (typeof e !== 'number') rec(name, i, e.stack); } } return s; }
  function inCatch(n, name) { for (let i = 0; i < n; i++) { try { throw i; } catch (e) { if (at(i)) rec(name, i, new Error('c').stack); } } }
  function inFinally(n, name) { for (let i = 0; i < n; i++) { try { try { throw i; } finally { if (at(i)) rec(name, i, new Error('f').stack); } } catch (e) {} } }
  function afterFinally(n, name) { for (let i = 0; i < n; i++) { try { try { throw i; } finally { i += 0; } } catch (e) { if (at(i)) rec(name, i, new Error('c').stack); } } }
  class K {
    constructor(n, name) { for (let i = 0; i < n; i++) { try { const r = leafThrow(i); if (r) rec(name, i, r); } catch (e) {} } }
    m(n, name) { for (let i = 0; i < n; i++) { try { const r = leafThrow(i); if (r) rec(name, i, r); } catch (e) {} } }
  }

  const scenarios = {
    calleeAfterCatch, calleeAfterFinally, calleeInFinally, calleeNoHandler, deepInline, lateThrow,
    lateThrowFinally, lateThrowAfterCatch, lateType, inCatch, inFinally, afterFinally,
  };
  function outer(f, n, name) { const r = f(n, name); return r; }

  if (mode === 'warm') {
    for (const k of Object.keys(scenarios)) for (let w = 0; w < 300; w++) outer(scenarios[k], 20, 'warm');
    for (let w = 0; w < 300; w++) new K(20, 'warm').m(20, 'warm');
  }
  for (const k of Object.keys(scenarios)) { outer(scenarios[k], N, k); outer(scenarios[k], N, k); }
  new K(N, 'ctor'); new K(N, 'ctor'); new K(10, 'warm').m(N, 'method'); new K(10, 'warm').m(N, 'method');
  for (let i = 0; i < N; i++) { try { const r = leafThrow(i); if (r) rec('toplevel', i, r); } catch (e) {} }
  for (let i = 0; i < N; i++) { try { try { throw i; } finally { if (at(i)) rec('toplevelFinally', i, new Error('t').stack); } } catch (e) { if (at(i)) rec('toplevelCatch', i, new Error('t').stack); } }
  console.log(JSON.stringify(out));
} else {
  const expected = {
    calleeAfterCatch: ['leafThrow', 'calleeAfterCatch', 'outer', '<top>'],
    calleeAfterFinally: ['leafThrow', 'calleeAfterFinally', 'outer', '<top>'],
    calleeInFinally: ['leafNew', 'calleeInFinally', 'outer', '<top>'],
    calleeNoHandler: ['leafNew', 'calleeNoHandler', 'outer', '<top>'],
    deepInline: ['leafNew', 'leafDeep', 'deepInline', 'outer', '<top>'],
    lateThrow: ['leafLate', 'lateThrow', 'outer', '<top>'],
    lateThrowFinally: ['leafLate', 'lateThrowFinally', 'outer', '<top>'],
    lateThrowAfterCatch: ['leafLate', 'lateThrowAfterCatch', 'outer', '<top>'],
    lateType: ['leafLateType', 'lateType', 'outer', '<top>'],
    inCatch: ['inCatch', 'outer', '<top>'],
    inFinally: ['inFinally', 'outer', '<top>'],
    afterFinally: ['afterFinally', 'outer', '<top>'],
    ctor: ['leafThrow', 'new K', '<top>'],
    method: ['leafThrow', 'm', '<top>'],
    toplevel: ['leafThrow', '<top>'],
    toplevelFinally: ['<top>'],
    toplevelCatch: ['<top>'],
  };

  const run = (flags, m) => {
    const child = spawnSync(process.execPath, [...flags, __filename, m], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
    assert.strictEqual(child.status, 0, `${m} ${flags.join(' ')}: ${child.stderr}`);
    return child.stdout;
  };

  for (const m of ['early', 'late', 'warm']) {
    const compiled = run([], m);
    const interpreted = run(['--jitless'], m);
    const records = JSON.parse(compiled);
    const seen = new Set();
    for (const [name, i, list] of records) {
      seen.add(name);
      assert.deepStrictEqual(list.map((f) => f.replace(/:\d+:\d+$/, '')), expected[name], `${m}: ${name} at ${i}`);
    }
    for (const name of Object.keys(expected)) assert.ok(seen.has(name), `${m}: ${name} captured nothing`);
    assert.strictEqual(compiled, interpreted, `${m}: compiled frames differ from the interpreter's`);
  }
  console.log('ok');
}
