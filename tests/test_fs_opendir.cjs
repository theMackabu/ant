const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function sortedNames(entries) {
  return entries.map((entry) => entry.name).sort();
}

async function main() {
  assert.strictEqual(typeof fs.opendir, 'function');
  assert.strictEqual(typeof fs.opendirSync, 'function');
  assert.strictEqual(typeof fsp.opendir, 'function');

  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-opendir-'));
  try {
    const expected = [];
    for (let i = 0; i < 40; i++) {
      const name = `file-${i}`;
      fs.writeFileSync(path.join(root, name), String(i));
      expected.push(name);
    }
    fs.mkdirSync(path.join(root, 'sub'));
    expected.push('sub');
    expected.sort();

    const dir = fs.opendirSync(root);
    assert.strictEqual(dir.path, root);

    const syncEntries = [];
    let entry;
    while ((entry = dir.readSync()) !== null) syncEntries.push(entry);
    assert.deepStrictEqual(sortedNames(syncEntries), expected);
    assert.strictEqual(dir.readSync(), null);

    const sub = syncEntries.find((e) => e.name === 'sub');
    assert.strictEqual(sub.isDirectory(), true);
    assert.strictEqual(sub.isFile(), false);
    assert.strictEqual(sub.parentPath, root);
    assert.strictEqual(syncEntries.find((e) => e.name === 'file-0').isFile(), true);

    dir.closeSync();
    assert.throws(() => dir.readSync(), { code: 'ERR_DIR_CLOSED' });
    assert.throws(() => dir.closeSync(), { code: 'ERR_DIR_CLOSED' });

    const small = fs.opendirSync(root, { bufferSize: 1 });
    const smallEntries = [];
    while ((entry = small.readSync()) !== null) smallEntries.push(entry);
    assert.deepStrictEqual(sortedNames(smallEntries), expected);
    small.closeSync();

    assert.throws(() => fs.opendirSync(root, { bufferSize: 0 }), RangeError);
    assert.throws(() => fs.opendirSync(path.join(root, 'missing')), (error) => {
      assert.strictEqual(error.code, 'ENOENT');
      assert.strictEqual(error.syscall, 'opendir');
      return true;
    });

    const promiseDir = await fsp.opendir(root);
    const iterated = [];
    for await (const item of promiseDir) iterated.push(item);
    assert.deepStrictEqual(sortedNames(iterated), expected);
    await assert.rejects(promiseDir.read(), { code: 'ERR_DIR_CLOSED' });

    const earlyExit = await fsp.opendir(root);
    for await (const item of earlyExit) {
      assert.strictEqual(typeof item.name, 'string');
      break;
    }
    assert.throws(() => earlyExit.readSync(), { code: 'ERR_DIR_CLOSED' });

    const readDir = await fsp.opendir(root);
    const first = await readDir.read();
    assert.strictEqual(expected.includes(first.name), true);
    await readDir.close();

    const bufferDir = fs.opendirSync(root, { encoding: 'buffer' });
    const bufferEntry = bufferDir.readSync();
    assert.strictEqual(Buffer.isBuffer(bufferEntry.name), true);
    bufferDir.closeSync();

    await new Promise((resolve, reject) => {
      const result = fs.opendir(root, (error, callbackDir) => {
        if (error) return reject(error);
        callbackDir.read((readError, item) => {
          if (readError) return reject(readError);
          try {
            assert.strictEqual(expected.includes(item.name), true);
          } catch (assertionError) {
            return reject(assertionError);
          }
          callbackDir.close((closeError) => closeError ? reject(closeError) : resolve());
        });
      });
      assert.strictEqual(result, undefined);
    });

    await new Promise((resolve, reject) => {
      fs.opendir(path.join(root, 'missing'), (error) => {
        try {
          assert.strictEqual(error && error.code, 'ENOENT');
          resolve();
        } catch (assertionError) {
          reject(assertionError);
        }
      });
    });

    const disposed = fs.opendirSync(root);
    disposed[Symbol.dispose]();
    disposed[Symbol.dispose]();
    assert.throws(() => disposed.readSync(), { code: 'ERR_DIR_CLOSED' });
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
