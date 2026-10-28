const assert = require('node:assert');

function churn() {
  const a = [];
  for (let i = 0; i < 1500000; i++) a.push({ i });
  return a.length;
}

function settle(done) {
  setTimeout(() => {
    churn();
    setTimeout(() => {
      churn();
      churn();
      done();
    }, 0);
  }, 0);
}

function use(o) {
  return o.get() + o.base;
}

function liveFunction() {
  const refs = [];
  let total = 0;
  for (let p = 0; p < 6; p++) {
    const proto = { base: 1, get() { return 2; }, payload: new Array(1000).fill(p) };
    refs.push(new WeakRef(proto));
    for (let k = 0; k < 40; k++) {
      const o = Object.create(proto);
      for (let i = 0; i < 2000; i++) total += use(o);
    }
  }
  assert.strictEqual(total, 6 * 40 * 2000 * 3);
  settle(() => console.log(`alive ${refs.filter((r) => r.deref() !== undefined).length}`));
}

function deadUnits() {
  const refs = [];
  let total = 0;
  for (let p = 0; p < 20; p++) {
    const fn = new Function('o', 'return o.get() + o.base;');
    const proto = { base: 1, get() { return 2; }, payload: new Array(1000).fill(p) };
    refs.push(new WeakRef(proto));
    for (let k = 0; k < 40; k++) {
      const o = Object.create(proto);
      for (let i = 0; i < 2000; i++) total += fn(o);
    }
  }
  assert.strictEqual(total, 20 * 40 * 2000 * 3);
  settle(() => console.log(`alive ${refs.filter((r) => r.deref() !== undefined).length}`));
}

function inlinedCallees() {
  let total = 0;
  function caller(f, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += f(i);
    return s;
  }
  for (let p = 0; p < 200; p++) {
    const f = new Function('x', `return (x & 7) + ${p};`);
    total += caller(f, 3000);
    if (p % 20 === 19) churn();
  }
  let expect = 0;
  for (let p = 0; p < 200; p++) expect += 3000 * p + 375 * 28;
  assert.strictEqual(total, expect);
  settle(() => console.log('inlined ok'));
}

const modes = { live: liveFunction, dead: deadUnits, inlined: inlinedCallees };

if (process.argv[2] in modes) modes[process.argv[2]]();
else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:off']) {
    for (const kind of Object.keys(modes)) {
      const child = spawnSync(process.execPath, [__filename, kind], {
        encoding: 'utf8',
        env: { ...process.env, ANT_DEBUG: `gc:trace${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
        timeout: 120000,
        maxBuffer: 64 * 1024 * 1024,
      });
      const label = `${kind} ${mode}`;
      assert.strictEqual(child.status, 0, `${label}: ${child.error || child.stdout + child.stderr.slice(-2000)}`);
      if (kind === 'inlined') {
        assert.match(child.stdout, /inlined ok/, label);
        const units = [...child.stderr.matchAll(/^gc: major .* units=(\d+)$/gm)].map((m) => Number(m[1]));
        assert.ok(units.length > 0, `${label}: no major collection traced`);
        assert.ok(units[units.length - 1] < 40, `${label}: ${units[units.length - 1]} code units alive at the end`);
        continue;
      }
      const alive = /alive (\d+)/.exec(child.stdout);
      assert.ok(alive, `${label}: ${child.stdout}`);
      assert.ok(Number(alive[1]) <= 1, `${label}: ${alive[1]} prototypes kept alive by compiled code`);
    }
  }
  console.log('ok');
}
