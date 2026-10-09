const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

async function main() {
  assert.strictEqual(typeof fs.link, 'function');
  assert.strictEqual(typeof fs.linkSync, 'function');
  assert.strictEqual(typeof fsp.link, 'function');

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-link-'));
  try {
    const target = path.join(root, 'target');
    fs.writeFileSync(target, 'ant');
    const ino = fs.statSync(target).ino;

    const syncLink = path.join(root, 'sync-link');
    assert.strictEqual(fs.linkSync(target, syncLink), undefined);
    assert.strictEqual(fs.statSync(syncLink).ino, ino);
    assert.strictEqual(fs.statSync(target).nlink, 2);
    assert.strictEqual(fs.lstatSync(syncLink).isSymbolicLink(), false);

    assert.throws(() => fs.linkSync(target, syncLink), (error) => {
      assert.strictEqual(error.code, 'EEXIST');
      assert.strictEqual(error.syscall, 'link');
      assert.strictEqual(error.path, target);
      assert.strictEqual(error.dest, syncLink);
      return true;
    });
    assert.throws(() => fs.linkSync(path.join(root, 'missing'), path.join(root, 'x')), { code: 'ENOENT' });

    const callbackLink = path.join(root, 'callback-link');
    await new Promise((resolve, reject) => {
      const result = fs.link(target, callbackLink, (error) => error ? reject(error) : resolve());
      assert.strictEqual(result, undefined);
    });
    assert.strictEqual(fs.statSync(callbackLink).ino, ino);

    await new Promise((resolve, reject) => {
      fs.link(target, callbackLink, (error) => {
        try {
          assert.strictEqual(error && error.code, 'EEXIST');
          resolve();
        } catch (assertionError) {
          reject(assertionError);
        }
      });
    });

    const promiseLink = path.join(root, 'promise-link');
    assert.strictEqual(await fsp.link(target, promiseLink), undefined);
    assert.strictEqual(fs.readFileSync(promiseLink, 'utf8'), 'ant');
    assert.strictEqual(fs.statSync(target).nlink, 4);

    await assert.rejects(fsp.link(target, promiseLink), { code: 'EEXIST' });

    const urlLink = path.join(root, 'url-link');
    fs.linkSync(new URL(`file://${target}`), Buffer.from(urlLink));
    assert.strictEqual(fs.statSync(urlLink).ino, ino);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
