const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform !== 'win32') {
  console.log('Windows recursive mkdir root test skipped outside Windows');
  process.exit(0);
}

fs.mkdirSync('\\', { recursive: true });

const currentDriveRoot = path.parse(process.cwd()).root;
assert.match(currentDriveRoot, /^[A-Za-z]:\\$/);
fs.mkdirSync(currentDriveRoot, { recursive: true });
if (fs.existsSync('C:\\')) fs.mkdirSync('C:\\', { recursive: true });

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-mkdirp-windows-'));
try {
  // The \\?\C:\ form exercises the same two-component root parser as
  // \\server\share without requiring an SMB share in the test environment.
  const extendedRoot = '\\\\?\\' + root;
  const nested = extendedRoot + '\\nested\\unc\\path';
  fs.mkdirSync(nested, { recursive: true });
  assert.strictEqual(fs.existsSync(path.join(root, 'nested', 'unc', 'path')), true);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('Windows recursive mkdir roots ok');
