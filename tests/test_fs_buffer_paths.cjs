const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const root = fs.mkdtempSync(Buffer.from(path.join(os.tmpdir(), 'ant-buffer-paths-')), { encoding: 'buffer' });
const at = name => Buffer.concat([root, Buffer.from(path.sep), Buffer.isBuffer(name) ? name : Buffer.from(name)]);
const call = (name, ...args) => new Promise((resolve, reject) => fs[name](...args, (err, value) => err ? reject(err) : resolve(value)));

async function main() {
  assert(Buffer.isBuffer(root));
  const file = at('data');
  fs.writeFileSync(file, 'abc');
  fs.appendFileSync(file, 'def');
  assert.strictEqual(fs.readFileSync(file, 'utf8'), 'abcdef');
  const padded = Buffer.concat([Buffer.from([0]), file, Buffer.from([0])]);
  const sliced = Buffer.from(padded.buffer, padded.byteOffset + 1, file.length);
  assert.strictEqual(fs.readFileSync(sliced, 'utf8'), 'abcdef');
  assert(fs.statSync(file).isFile());
  assert(fs.lstatSync(file).isFile());
  assert(fs.existsSync(file));
  fs.accessSync(file);
  fs.chmodSync(file, 0o600);
  fs.utimesSync(file, 1, 2);
  fs.truncateSync(file, 3);
  const fd = fs.openSync(file, 'r');
  const data = Buffer.alloc(3);
  assert.strictEqual(fs.readSync(fd, data, 0, 3, null), 3);
  fs.closeSync(fd);
  assert.strictEqual(data.toString(), 'abc');
  fs.copyFileSync(file, at('copy'));
  fs.renameSync(at('copy'), at('renamed'));
  fs.cpSync(file, at('cp'));
  fs.unlinkSync(at('renamed'));
  fs.mkdirSync(at('dir'));
  fs.rmdirSync(at('dir'));
  fs.symlinkSync(Buffer.from('data'), at('link'));
  assert.strictEqual(fs.readlinkSync(at('link'), 'buffer').toString(), 'data');
  assert(Buffer.isBuffer(fs.readlinkSync(at('link'), { encoding: 'buffer' })));
  assert(Buffer.isBuffer(fs.realpathSync(file, 'buffer')));
  assert(Buffer.isBuffer(fs.realpathSync.native(file, { encoding: 'buffer' })));
  assert(fs.readdirSync(root, 'buffer').every(Buffer.isBuffer));
  const entries = fs.readdirSync(root, { encoding: 'buffer', withFileTypes: true });
  assert(entries.every(e => Buffer.isBuffer(e.name)));
  assert(entries.find(e => e.name.toString() === 'link').isSymbolicLink());
  for (const method of ['statSync', 'lstatSync']) {
    assert.strictEqual(fs[method](at('missing'), { throwIfNoEntry: false }), undefined);
    assert.throws(() => fs[method](at('missing')));
  }
  assert.throws(() => fs.writeFileSync(Buffer.concat([file, Buffer.from([0, 120])]), 'bad'), TypeError);
  assert.strictEqual(fs.readFileSync(file, 'utf8'), 'abc');
  // POSIX filenames are bytes, not necessarily valid UTF-8.
  if (process.platform === 'linux') {
    const name = Buffer.from([0xff, 0xfe]);
    fs.writeFileSync(at(name), 'raw');
    assert(fs.readdirSync(root, { encoding: 'buffer' }).some(n => n.equals(name)));
    assert.strictEqual(fs.readFileSync(at(name), 'utf8'), 'raw');
  }
  for (const api of [fsp, Object.fromEntries(['writeFile', 'readFile', 'stat', 'lstat', 'access', 'chmod', 'copyFile', 'cp', 'rename', 'mkdir', 'rmdir', 'unlink', 'symlink', 'readlink', 'realpath', 'readdir', 'mkdtemp', 'rm'].map(name => [name, (...args) => call(name, ...args)]))]) {
    await api.writeFile(at('async'), 'async');
    assert.strictEqual(await api.readFile(at('async'), 'utf8'), 'async');
    assert((await api.stat(at('async'))).isFile());
    assert((await api.lstat(at('async'))).isFile());
    await api.access(at('async'));
    await api.chmod(at('async'), 0o600);
    await api.copyFile(at('async'), at('async-copy'));
    if (typeof Ant !== 'undefined') await api.cp(at('async'), at('async-cp'));
    else await api.copyFile(at('async'), at('async-cp'));
    await api.rename(at('async-copy'), at('async-renamed'));
    await api.mkdir(at('async-dir'));
    await api.rmdir(at('async-dir'));
    await api.symlink(Buffer.from('async'), at('async-link'));
    assert(Buffer.isBuffer(await api.readlink(at('async-link'), 'buffer')));
    assert(Buffer.isBuffer(await api.realpath(at('async'), { encoding: 'buffer' })));
    assert((await api.readdir(root, 'buffer')).every(Buffer.isBuffer));
    assert((await api.readdir(root, { encoding: 'buffer', withFileTypes: true })).every(e => Buffer.isBuffer(e.name)));
    const tmp = await api.mkdtemp(at('tmp-'), 'buffer');
    assert(Buffer.isBuffer(tmp));
    await api.rm(tmp, { recursive: true });
    for (const name of ['async', 'async-cp', 'async-renamed', 'async-link']) await api.unlink(at(name));
  }
  const handle = await fsp.open(file, 'r');
  await handle.close();
  const asyncFd = await call('open', file, 'r');
  await call('close', asyncFd);
  const exists = await new Promise(resolve => fs.exists(file, resolve));
  assert.strictEqual(exists, true);
  await new Promise((resolve, reject) => {
    const writer = fs.createWriteStream(at('stream'));
    writer.on('error', reject);
    writer.on('close', resolve);
    writer.end('stream');
  });
  await new Promise((resolve, reject) => {
    let contents = '';
    const reader = fs.createReadStream(at('stream'));
    reader.on('error', reject);
    reader.on('data', chunk => { contents += chunk.toString(); });
    reader.on('end', () => { assert.strictEqual(contents, 'stream'); resolve(); });
  });
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { watcher.close(); reject(new Error('watch event timed out')); }, 2000);
    const watcher = fs.watch(root, { persistent: false, encoding: 'buffer' }, (event, filename) => {
      clearTimeout(timeout);
      watcher.close();
      try { assert(Buffer.isBuffer(filename)); resolve(); } catch (error) { reject(error); }
    });
    fs.writeFileSync(at('watched'), 'changed');
  });
  const listener = () => {};
  const watchPath = typeof Ant !== 'undefined' ? file : file.toString();
  fs.watchFile(watchPath, { persistent: false }, listener);
  fs.unwatchFile(watchPath, listener);
  console.log('fs:buffer-paths:ok');
}
main().catch(error => { console.error(error.stack); process.exitCode = 1; }).finally(() => fs.rmSync(root, { recursive: true, force: true }));
