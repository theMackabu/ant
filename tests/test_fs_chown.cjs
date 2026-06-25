const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const { promisify } = require('node:util');

const testFile = path.join(__dirname, '.fs_chown_tmp');

(async () => {
  try {
    fs.writeFileSync(testFile, 'chown test');
    const { uid, gid } = fs.statSync(testFile);

    assert.strictEqual(typeof fs.chown, 'function');
    assert.strictEqual(typeof fs.chownSync, 'function');
    assert.strictEqual(typeof fsp.chown, 'function');

    fs.chownSync(testFile, uid, gid);
    await fsp.chown(testFile, uid, gid);
    await promisify(fs.chown)(testFile, uid, gid);

    await new Promise((resolve, reject) => {
      fs.chown(testFile, uid, gid, error => error ? reject(error) : resolve());
    });

    console.log('fs chown test passed');
  } finally {
    try {
      fs.unlinkSync(testFile);
    } catch {}
  }
})().catch(error => {
  console.error(error);
  process.exit(1);
});
