const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
if (process.platform === 'win32') {
  console.log('skip: native addon fixture requires a Unix compiler');
  process.exit(0);
}
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-napi-clear-'));
try {
  const addon = path.join(directory, 'binding.node');
  const source = path.join(__dirname, 'fixtures/napi-clear-exception/binding.c');
  const flags = process.platform === 'darwin' ? ['-bundle', '-undefined', 'dynamic_lookup'] : ['-shared', '-fPIC'];
  const build = spawnSync('cc', [...flags, source, '-o', addon], { encoding: 'utf8', timeout: 30000 });
  assert.strictEqual(build.status, 0, build.stderr);
  const { clear } = require(addon);
  for (let i = 0; i < 3; i++) {
    for (const reason of [undefined, null, 'exception', new Error('exception'), {}]) {
      assert.strictEqual(clear(reason), reason, 'cleared exception must retain identity');
    }
  }
  console.log('N-API clear exception ok');
} finally { fs.rmSync(directory, { recursive: true, force: true }); }
