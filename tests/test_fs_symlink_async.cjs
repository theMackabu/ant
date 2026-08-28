const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

async function callbackSymlink(target, link, type) {
  await new Promise((resolve, reject) => {
    const callback = (error) => error ? reject(error) : resolve();
    const result = type === undefined
      ? fs.symlink(target, link, callback)
      : fs.symlink(target, link, type, callback);
    assert.strictEqual(result, undefined);
  });
}

async function main() {
  assert.strictEqual(typeof fs.symlink, 'function');
  assert.strictEqual(typeof fs.promises.symlink, 'function');
  assert.strictEqual(typeof fsp.symlink, 'function');

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-symlink-async-'));
  try {
    const target = path.join(root, 'target');
    fs.writeFileSync(target, 'ant');

    const callbackLink = path.join(root, 'callback-link');
    await callbackSymlink(target, callbackLink);
    assert.strictEqual(fs.readlinkSync(callbackLink), target);

    const typedCallbackLink = path.join(root, 'typed-callback-link');
    await callbackSymlink(target, typedCallbackLink, 'file');
    assert.strictEqual(fs.readFileSync(typedCallbackLink, 'utf8'), 'ant');

    const promiseLink = path.join(root, 'promise-link');
    await fsp.symlink(target, promiseLink, 'file');
    assert.strictEqual(fs.readlinkSync(promiseLink), target);

    const promisesPropertyLink = path.join(root, 'promises-property-link');
    await fs.promises.symlink(target, promisesPropertyLink);
    assert.strictEqual(fs.readFileSync(promisesPropertyLink, 'utf8'), 'ant');

    let promiseError;
    try {
      await fsp.symlink(target, promiseLink);
    } catch (error) {
      promiseError = error;
    }
    assert.strictEqual(promiseError && promiseError.code, 'EEXIST');

    await new Promise((resolve, reject) => {
      fs.symlink(target, callbackLink, (error) => {
        try {
          assert.strictEqual(error && error.code, 'EEXIST');
          resolve();
        } catch (assertionError) {
          reject(assertionError);
        }
      });
    });
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
