const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const show = (root) => (entry) => `${entry.name}|${String(entry.parentPath).replace(root, 'R')}|${entry.isDirectory()}`;

async function readAll(dir) {
  const out = [];
  let entry;
  while ((entry = await dir.read()) !== null) out.push(entry);
  return out;
}

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-dir-async-'));
  try {
    fs.mkdirSync(path.join(root, 'a', 'b'), { recursive: true });
    fs.writeFileSync(path.join(root, 'f'), '');
    fs.writeFileSync(path.join(root, 'a', 'g'), '');
    fs.writeFileSync(path.join(root, 'a', 'b', 'h'), '');
    fs.symlinkSync(path.join(root, 'a'), path.join(root, 'lnk'));
    const expected = ['a|R|true', 'f|R|false', 'g|R/a|false', 'b|R/a|true', 'h|R/a/b|false', 'lnk|R|false'].sort();

    // readdir withFileTypes carries parentPath in the argument's own type
    assert.deepStrictEqual(
      fs.readdirSync(root, { withFileTypes: true }).map(show(root)).sort(),
      ['a|R|true', 'f|R|false', 'lnk|R|false']
    );
    const bufferParent = fs.readdirSync(Buffer.from(root), { withFileTypes: true })[0].parentPath;
    assert.ok(Buffer.isBuffer(bufferParent));
    assert.strictEqual(bufferParent.toString(), root);
    assert.strictEqual(fs.readdirSync(new URL(`file://${root}`), { withFileTypes: true })[0].parentPath, root);
    assert.strictEqual((await fsp.readdir(root, { withFileTypes: true }))[0].parentPath, root);
    assert.ok(Buffer.isBuffer((await fsp.readdir(Buffer.from(root), { withFileTypes: true }))[0].parentPath));
    assert.strictEqual('path' in fs.readdirSync(root, { withFileTypes: true })[0], false);

    // recursive Dir: breadth-first, symlinks not followed, path.join'd parents
    const syncDir = fs.opendirSync(root, { recursive: true });
    const syncEntries = [];
    let entry;
    while ((entry = syncDir.readSync()) !== null) syncEntries.push(entry);
    syncDir.closeSync();
    assert.deepStrictEqual(syncEntries.map(show(root)).sort(), expected);
    assert.deepStrictEqual(
      syncEntries.map((e) => e.name).slice(-3),
      ['g', 'b', 'h'].filter((name) => syncEntries.slice(-3).some((e) => e.name === name))
    );

    process.chdir(root);
    const relative = fs.opendirSync('./', { recursive: true });
    const relativeEntries = [];
    while ((entry = relative.readSync()) !== null) relativeEntries.push(`${entry.name}|${entry.parentPath}`);
    relative.closeSync();
    assert.ok(relativeEntries.includes('a|./'));
    assert.ok(relativeEntries.includes('g|a'));
    assert.ok(relativeEntries.includes('h|a/b'));

    for (const bufferSize of [1, 2, 32]) {
      const asyncDir = await fsp.opendir(root, { recursive: true, bufferSize });
      assert.deepStrictEqual((await readAll(asyncDir)).map(show(root)).sort(), expected);
      assert.strictEqual(await asyncDir.read(), null);
      await asyncDir.close();
    }

    const iterated = [];
    for await (const item of await fsp.opendir(root, { recursive: true })) iterated.push(item);
    assert.deepStrictEqual(iterated.map(show(root)).sort(), expected);

    // async operations queue in order; sync work during them is rejected
    const queued = await fsp.opendir(root);
    const first = queued.read();
    const second = queued.read();
    const closing = queued.close();
    assert.throws(() => queued.readSync(), { code: 'ERR_DIR_CONCURRENT_OPERATION' });
    assert.throws(() => queued.closeSync(), { code: 'ERR_DIR_CONCURRENT_OPERATION' });
    const [a, b] = await Promise.all([first, second]);
    assert.notStrictEqual(a.name, b.name);
    assert.strictEqual(await closing, undefined);
    await assert.rejects(queued.read(), { code: 'ERR_DIR_CLOSED' });
    await assert.rejects(queued.close(), { code: 'ERR_DIR_CLOSED' });
    assert.throws(() => queued.readSync(), { code: 'ERR_DIR_CLOSED' });

    // a read queued behind close() sees the closed handle
    const lateRead = await fsp.opendir(root);
    const closed = lateRead.close();
    await assert.rejects(lateRead.read(), { code: 'ERR_DIR_CLOSED' });
    await closed;

    // iterator break and return() close the handle
    const broken = await fsp.opendir(root, { recursive: true });
    for await (const item of broken) { assert.ok(item.name); break; }
    await assert.rejects(broken.read(), { code: 'ERR_DIR_CLOSED' });

    const manual = await fsp.opendir(root);
    const iterator = manual[Symbol.asyncIterator]();
    assert.strictEqual((await iterator.next()).done, false);
    assert.deepStrictEqual(await iterator.return(42), { value: 42, done: true });
    await assert.rejects(manual.read(), { code: 'ERR_DIR_CLOSED' });

    const disposed = await fsp.opendir(root);
    await disposed[Symbol.asyncDispose]();
    await disposed[Symbol.asyncDispose]();
    await assert.rejects(disposed.read(), { code: 'ERR_DIR_CLOSED' });

    // callback forms
    await new Promise((resolve, reject) => {
      fs.opendir(root, { recursive: true }, (error, dir) => {
        if (error) return reject(error);
        dir.read((readError, item) => {
          if (readError) return reject(readError);
          try { assert.ok(item.name); } catch (assertionError) { return reject(assertionError); }
          dir.close((closeError) => closeError ? reject(closeError) : resolve());
        });
      });
    });

    // async errors use Node's "CODE: message, syscall 'path' -> 'dest'" format
    const R = (message) => message.split(root).join('R');
    const linkError = await fsp.link(path.join(root, 'f'), path.join(root, 'f')).catch((e) => e);
    assert.strictEqual(linkError.constructor, Error);
    assert.strictEqual(R(linkError.message), "EEXIST: file already exists, link 'R/f' -> 'R/f'");
    assert.strictEqual(linkError.syscall, 'link');
    assert.strictEqual(linkError.code, 'EEXIST');

    const symlinkError = await fsp.symlink('tgt', path.join(root, 'f')).catch((e) => e);
    assert.strictEqual(R(symlinkError.message), "EEXIST: file already exists, symlink 'tgt' -> 'R/f'");
    assert.strictEqual(symlinkError.path, 'tgt');
    assert.strictEqual(R(symlinkError.dest), 'R/f');
    assert.throws(() => fs.symlinkSync('tgt', path.join(root, 'f')), (error) => {
      assert.strictEqual(error.path, 'tgt');
      assert.strictEqual(R(error.dest), 'R/f');
      return true;
    });

    const cases = [
      [() => fsp.rename(path.join(root, 'nope'), path.join(root, 'x')), "ENOENT: no such file or directory, rename 'R/nope' -> 'R/x'", 'rename'],
      [() => fsp.readFile(path.join(root, 'nope')), "ENOENT: no such file or directory, open 'R/nope'", 'open'],
      [() => fsp.readdir(path.join(root, 'nope')), "ENOENT: no such file or directory, scandir 'R/nope'", 'scandir'],
      [() => fsp.mkdir(path.join(root, 'a')), "EEXIST: file already exists, mkdir 'R/a'", 'mkdir'],
      [() => fsp.stat(path.join(root, 'nope')), "ENOENT: no such file or directory, stat 'R/nope'", 'stat'],
      [() => fsp.opendir(path.join(root, 'nope')), "ENOENT: no such file or directory, opendir 'R/nope'", 'opendir'],
      [() => new Promise((resolve) => fs.unlink(path.join(root, 'nope'), resolve)).then((e) => { throw e; }), "ENOENT: no such file or directory, unlink 'R/nope'", 'unlink'],
    ];
    for (const [start, message, syscall] of cases) {
      const error = await start().catch((e) => e);
      assert.strictEqual(R(error.message), message);
      assert.strictEqual(error.syscall, syscall);
    }
  } finally {
    process.chdir(os.tmpdir());
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
