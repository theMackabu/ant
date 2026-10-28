const assert = require('node:assert');

const GENERATIONS = 60;

function churn() {
  const a = [];
  for (let i = 0; i < 300000; i++) a.push({ i });
  return a.length;
}

if (process.argv[2] === 'child') {
  let total = 0, expected = 0;
  const refs = [];
  for (let g = 0; g < GENERATIONS; g++) {
    const f = new Function('o', `return o.x + ${g};`);
    refs.push(new WeakRef(f));
    const drive = new Function('f', 'o', 'let s = 0; for (let i = 0; i < 4096; i++) s += (i & 255) === 0 ? f(o) : 1; return s;');
    for (let r = 0; r < 3; r++) {
      total += drive(f, { x: 1 });
      expected += 4080 + 16 * (1 + g);
    }
    total += drive(f, { y: 0, x: 2 });
    expected += 4080 + 16 * (2 + g);
    if (g % 4 === 3) churn();
  }
  assert.strictEqual(total, expected);
  setTimeout(() => {
    churn();
    setTimeout(() => {
      churn();
      console.log(`total ${total} collected ${refs.filter((r) => r.deref() === undefined).length}`);
    }, 0);
  }, 0);
} else {
  const { spawnSync } = require('node:child_process');
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  for (const mode of ['default', 'opt:force', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: `dump/vm:op-warn${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 64 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr.slice(-2000)}`);
    assert.match(child.stdout, /total \d+/, `${mode}: ${child.stdout}`);
    if (tier && mode !== 'opt:off') {
      const exits = child.stderr.match(/jit: bailout .* kind=(?:BadShape inlined_in=|Hoisted)/g) || [];
      assert.strictEqual(exits.length, GENERATIONS, `${mode}: ${exits.length} first compiles speculated on their fresh callee`);
      const collected = Number(/collected (\d+)/.exec(child.stdout)[1]);
      assert.ok(collected >= GENERATIONS / 2, `${mode}: only ${collected} callees collected`);
    }
  }
  console.log('ok');
}
