const assert = require('node:assert');
const fs = require('node:fs');

const COUNT = 80;
const ROUNDS = 3;
const TRIPS = 3000;

function runChild() {
  const fns = [];
  for (let i = 0; i < COUNT; i++)
    fns.push(new Function(`function g(o) { return o.x + ${i}; }
return function f${i}(o) { let s = 0; for (let k = 0; k < ${TRIPS}; k++) s += g(o); return s; };`)());
  let total = 0;
  for (let i = 0; i < COUNT; i++) for (let r = 0; r < ROUNDS; r++) total += fns[i]({ x: 1 });
  console.log(`total ${total}`);
}

if (process.argv[2] === 'child') {
  runChild();
} else {
  const { spawnSync } = require('node:child_process');
  let expected = 0;
  for (let i = 0; i < COUNT; i++) expected += ROUNDS * TRIPS * (1 + i);
  const guard = '/usr/lib/libgmalloc.dylib';
  const extra = process.platform === 'darwin' && fs.existsSync(guard) ? { DYLD_INSERT_LIBRARIES: guard } : {};
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  for (const mode of ['default', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ...extra, ANT_DEBUG: `dump/vm:op-warn${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
      timeout: 120000, maxBuffer: 16 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr}`);
    assert.match(child.stdout, new RegExp(`total ${expected}\\b`), `${mode}: result`);
    if (tier && mode === 'default') {
      const built = child.stderr.match(/jit: opt (compiled|codegen-failed) func=f\d+ /g) || [];
      assert.ok(built.length >= 40, `${mode}: ${built.length} opt builds`);
      assert.match(child.stderr, /jit: opt compiled func=f\d+ .*inlined=1/, `${mode}: the callee is inlined`);
    }
    console.log(`PASS opt state table ${mode}`);
  }
}
