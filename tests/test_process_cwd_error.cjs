const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

assert.strictEqual(typeof process.cwd(), 'string');
if (process.platform === 'win32') {
  console.log('cwd failure test skipped: Windows does not allow removing cwd');
} else {
  const previous = process.cwd();
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cwd-error-'));
  try {
    process.chdir(directory);
    fs.rmdirSync(directory);
    for (const operation of [() => process.cwd(), () => path.resolve('file'), () => path.relative('.', 'file')]) {
      assert.throws(operation, error =>
        error instanceof Error && error.code === 'ENOENT' &&
        error.syscall === 'uv_cwd' && typeof error.errno === 'number' && error.errno < 0
      );
    }
  } finally {
    process.chdir(previous);
    if (fs.existsSync(directory)) fs.rmdirSync(directory);
  }
  console.log('process.cwd failure metadata and path propagation ok');
}
