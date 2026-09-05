const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const previous = process.cwd();
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cwd-cache-'));
const child = path.join(directory, 'child');
fs.mkdirSync(child);
try {
  process.chdir(directory);
  const current = fs.realpathSync(directory);
  for (let i = 0; i < 30000; i++) assert.strictEqual(process.cwd(), current);
  assert.strictEqual(path.resolve('file'), path.join(current, 'file'));
  assert.throws(() => process.chdir(path.join(directory, 'missing')));
  assert.strictEqual(process.cwd(), current);
  for (let i = 0; i < 100000; i++) String(i).repeat(20);
  assert.strictEqual(process.cwd(), current);
  process.chdir('child');
  assert.strictEqual(process.cwd(), fs.realpathSync(child));
  process.chdir('..');
  assert.strictEqual(process.cwd(), current);
  if (process.platform !== 'win32') {
    process.chdir(child);
    const cached = process.cwd();
    fs.rmdirSync(child);
    // Node retains a successful cached value until a successful chdir.
    assert.strictEqual(process.cwd(), cached);
    assert.strictEqual(path.resolve('file'), path.join(cached, 'file'));
  }
} finally {
  process.chdir(previous);
  if (fs.existsSync(child)) fs.rmdirSync(child);
  fs.rmdirSync(directory);
}
assert.strictEqual(process.cwd(), previous);
console.log('cwd cache, invalidation, allocation churn and deleted cached cwd ok');
