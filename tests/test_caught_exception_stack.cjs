const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-caught-stack-'));
try {
  for (const warmup of [0, 150]) {
    const file = path.join(root, `catch-${warmup}.cjs`);
    fs.writeFileSync(file, `
      const { execFile } = require('node:child_process');
      function capture(action) {
        try { ${warmup === 0 ? 'Reflect.apply(action, null, [])' : 'action()'}; }
        catch (value) { return value; }
      }
      function handled() { throw 'handled-marker-unique'; }
      for (let i = 0; i < ${warmup}; i++) capture(() => {});
      execFile(process.execPath, ['-e', '0'], () => {
        capture(handled);
        throw new Error('actual-callback-failure');
      });
    `);
    const result = spawnSync(process.execPath, [file], {
      encoding: 'utf8', timeout: 5000,
    });
    const stderr = result.stderr.replace(/\x1b\[[0-9;]*m/g, '');
    assert.strictEqual(result.status, 1, JSON.stringify(result));
    assert.match(stderr, /actual-callback-failure/);
    assert.doesNotMatch(stderr, /handled-marker-unique/);
  }
  for (const body of [
    "Reflect.apply(() => { throw 'handled-async-marker'; }, null, []);",
    "await Promise.resolve(); throw 'handled-async-marker';",
  ]) {
    const file = path.join(root, 'async.cjs');
    fs.writeFileSync(file, `
      const { execFile } = require('node:child_process');
      (async () => { ${body} })().catch(() => {
        execFile(process.execPath, ['-e', '0'], () => {
          throw new Error('actual-callback-failure');
        });
      });
    `);
    const result = spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 5000 });
    const stderr = result.stderr.replace(/\x1b\[[0-9;]*m/g, '');
    assert.strictEqual(result.status, 1, JSON.stringify(result));
    assert.match(stderr, /actual-callback-failure/);
    assert.doesNotMatch(stderr, /handled-async-marker/);
  }

  const stacklessFile = path.join(root, 'stackless-callback.cjs');
  fs.writeFileSync(stacklessFile, `
    const { execFile } = require('node:child_process');
    let original;
    function overflow() { return 1 + overflow(); }
    try { overflow(); } catch (error) { original = error; }
    Object.defineProperty(original, 'stack', {
      get() { console.log('STACK_GETTER_READ'); return 'wrong object stack'; },
    });
    execFile(process.execPath, ['-e', '0'], () => { throw original; });
  `);
  const stackless = spawnSync(process.execPath, [stacklessFile], { encoding: 'utf8', timeout: 5000 });
  assert.strictEqual(stackless.status, 1, JSON.stringify(stackless));
  assert.match(stackless.stderr, /Maximum (?:JIT )?call stack size exceeded/);
  assert.doesNotMatch(stackless.stdout, /STACK_GETTER_READ/);
  assert.doesNotMatch(stackless.stderr, /wrong object stack/);

} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
console.log('Caught exception stack cleanup ok');
