const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-symlink-sync-'));
try {
  const target = path.join(root, 'target');
  const link = path.join(root, 'link');
  fs.writeFileSync(target, 'ant');
  fs.symlinkSync(target, link, 'file');
  assert.equal(fs.readlinkSync(link), target);
  assert.equal(fs.readFileSync(link, 'utf8'), 'ant');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
