const assert = require('node:assert');

function child() {
  const out = [];
  const hot = (fn, rounds = 300) => { for (let r = 0; r < rounds; r++) fn(); };

  function sumValues(m) { let s = 0; for (const v of m.values()) s += v; return s; }
  function sumKeys(m) { let s = 0; for (const k of m.keys()) s += k; return s; }
  function sumSet(set) { let s = 0; for (const v of set) s += v; return s; }
  function sumEntries(m) { let s = 0; for (const [k, v] of m) s += k * v; return s; }
  function untilFifty(m) { let s = 0; for (const v of m.values()) { if (v > 50) break; s += v; } return s; }
  function pairs(m) { let s = 0; for (const a of m.values()) for (const b of m.values()) s += a * b; return s; }

  const map = new Map(), set = new Set();
  for (let i = 0; i < 100; i++) { map.set(i, i); set.add(i * 2); }
  hot(() => { sumValues(map); sumKeys(map); sumSet(set); sumEntries(map); untilFifty(map); });
  out.push('basic ' + [sumValues(map), sumKeys(map), sumSet(set), sumEntries(map), untilFifty(map)].join(' '));

  const six = [[1, 1], [2, 2], [3, 3], [4, 4], [5, 5], [6, 6]];
  function mutate(m) {
    let s = 0, n = 0;
    for (const v of m.values()) { s += v; if (n++ < 3) m.set(1000 + n, n); if (v === 5) m.delete(6); if (v === 2) m.delete(1); }
    return s;
  }
  hot(() => mutate(new Map(six)));
  const mutated = new Map(six);
  out.push('mutate ' + mutate(mutated) + ':' + [...mutated.keys()].join(',') + ' pairs ' + pairs(new Map(six)));

  function setGrow(st) { let s = 0; for (const v of st) { s += v; if (v < 5) st.add(v + 10); if (v === 3) st.delete(4); } return s + ':' + st.size; }
  hot(() => setGrow(new Set([1, 2, 3, 4])));
  out.push('set ' + setGrow(new Set([1, 2, 3, 4])));

  function clearing(m) { let s = 0; for (const v of m.values()) { s += v; if (v === 2) m.clear(); } return s; }
  hot(() => clearing(new Map(six)));
  out.push('clear ' + clearing(new Map(six)));

  function shared(m) {
    const it = m.values();
    let s = 0;
    for (const v of it) { s += v; const peek = it.next(); if (!peek.done) s += peek.value * 100; }
    return s;
  }
  hot(() => shared(new Map(six)));
  out.push('shared ' + shared(new Map(six)));

  function anyValues(x) { let s = 0; for (const v of x.values()) s += typeof v === 'number' ? v : 0; return s; }
  hot(() => anyValues(map));
  out.push('mixed ' + [anyValues(new Map([[1, 2]])), anyValues(new Set([3])), anyValues([4, 5]), anyValues(new Map())].join(' '));

  const resumed = map.values();
  resumed.next();
  resumed.next();
  function drain(it) { let s = 0; for (const v of it) s += v; return s; }
  out.push('resumed ' + drain(resumed) + ' ' + sumValues(new Map([[1, 'a'], [2, 'b']])));

  const objects = new Map();
  for (let i = 0; i < 64; i++) objects.set('k' + i, { alive: (i & 3) === 0, n: i });
  function flip(m) { let alive = 0; for (const o of m.values()) { o.alive = !o.alive; if (o.alive) alive += o.n; } return alive; }
  let flips = 0;
  hot(() => { flips += flip(objects); });
  out.push('objects ' + flips);

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
    timeout: 180000,
    maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  for (const [name, debug] of [['default', 'dump/vm:op-warn'], ['force', 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    if (name === 'default' && /jit: opt compiled/.test(child.stderr))
      for (const fn of ['sumValues', 'sumKeys', 'sumSet', 'untilFifty', 'mutate', 'setGrow', 'clearing', 'shared', 'flip'])
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${fn} must reach the opt tier`);
  }
  const graphs = run({ ANT_DEBUG: 'dump/opt:graph' });
  assert.strictEqual(graphs.status, 0, graphs.stderr.slice(-4000));
  const dumped = graphs.stdout + graphs.stderr;
  if (/=== optimized/.test(dumped)) {
    assert.match(dumped, /CollectionIteratorStep Map/, 'Map iteration must use the inline step');
    assert.match(dumped, /CollectionIteratorStep Set/, 'Set iteration must use the inline step');
  }
  console.log('ok');
}
