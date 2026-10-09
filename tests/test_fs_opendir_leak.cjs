const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

// Unclosed Dir handles must not make opendir fail with EMFILE: their
// finalizers close them, and opendir collects and retries when descriptors
// run out. (Node fails this loop; Ant goes further.)
if (process.platform === 'win32') {
  console.log('skip: needs a POSIX shell to lower the fd limit');
  process.exit(0);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-opendir-leak-'));
try {
  const script = path.join(root, 'leak.cjs');
  fs.writeFileSync(script, `
    const fs = require('node:fs');
    const dir = ${JSON.stringify(root)};
    for (let i = 0; i < 2000; i++) {
      const handle = fs.opendirSync(dir);
      handle.readSync();
      if (i % 4 === 0) handle.closeSync();
    }
    console.log('done');
  `);

  const result = spawnSync('/bin/sh', ['-c', 'ulimit -n 128 && exec "$0" "$1"', process.execPath, script], {
    encoding: 'utf8',
  });

  assert.strictEqual(result.stderr, '');
  assert.strictEqual(result.stdout.trim(), 'done');
  assert.strictEqual(result.status, 0);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
