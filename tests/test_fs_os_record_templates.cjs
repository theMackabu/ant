const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const statKeys = [
  'dev', 'ino', 'size', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'blksize', 'blocks',
  'atimeMs', 'mtimeMs', 'ctimeMs', 'birthtimeMs', 'atime', 'mtime', 'ctime', 'birthtime'
];
const dateKeys = ['atime', 'mtime', 'ctime', 'birthtime'];
const timeKeys = ['user', 'nice', 'sys', 'idle', 'irq'];

function checkRecord(record, keys) {
  assert.deepStrictEqual(Object.keys(record), keys);
  for (const key of keys) {
    const descriptor = Object.getOwnPropertyDescriptor(record, key);
    assert.strictEqual(descriptor.writable, true, key);
    assert.strictEqual(descriptor.enumerable, true, key);
    assert.strictEqual(descriptor.configurable, true, key);
    assert.strictEqual(descriptor.get, undefined, key);
  }
}

function checkStats(stat, size) {
  checkRecord(stat, statKeys);
  assert.strictEqual(stat.size, size);
  assert.strictEqual(stat.isFile(), true);
  assert.strictEqual(stat.isDirectory(), false);
  for (const key of statKeys.slice(0, 14)) assert.strictEqual(typeof stat[key], 'number', key);
  for (const key of dateKeys) {
    assert(stat[key] instanceof Date);
    assert.strictEqual(stat[key].getTime(), Math.round(stat[`${key}Ms`]));
  }
  assert.notStrictEqual(stat.atime, stat.mtime);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-record-templates-'));
const file = path.join(dir, 'file');
let handle;

function cleanup() {
  if (handle) fs.closeSync(handle.fd);
  fs.unlinkSync(file);
  fs.rmdirSync(dir);
}

async function main() {
  fs.writeFileSync(file, 'hello');
  const first = fs.statSync(file);
  const proto = Object.getPrototypeOf(first);
  checkStats(first, 5);
  first.atime.setTime(0);
  delete first.size;
  Object.defineProperty(first, 'dev', { get() { return -1; } });
  Object.setPrototypeOf(first, { changed: true });

  handle = await fsp.open(file, 'r+');
  for (const stat of [fs.statSync(file), fs.lstatSync(file), fs.fstatSync(handle.fd),
    await fsp.stat(file), await fsp.lstat(file), await handle.stat()]) {
    checkStats(stat, 5);
    assert.strictEqual(Object.getPrototypeOf(stat), proto);
    for (const key of dateKeys) assert.notStrictEqual(stat[key], first[key]);
  }
  assert.strictEqual(fs.statSync(dir).isDirectory(), true);

  const buffer = Buffer.alloc(5);
  const read = await handle.read(buffer, 0, 5, 0);
  const readProto = Object.getPrototypeOf(read);
  checkRecord(read, ['bytesRead', 'buffer']);
  assert.strictEqual(read.bytesRead, 5);
  assert.strictEqual(read.buffer, buffer);
  assert.strictEqual(buffer.toString(), 'hello');
  delete read.bytesRead;
  Object.defineProperty(read, 'buffer', { get() { return null; } });
  Object.setPrototypeOf(read, { changed: true });
  const nextRead = await handle.read(buffer, 0, 5, 5);
  checkRecord(nextRead, ['bytesRead', 'buffer']);
  assert.strictEqual(nextRead.bytesRead, 0);
  assert.strictEqual(nextRead.buffer, buffer);
  assert.strictEqual(Object.getPrototypeOf(nextRead), readProto);

  const write = await handle.write(buffer, 0, 5, 0);
  checkRecord(write, ['bytesWritten', 'buffer']);
  assert.strictEqual(write.bytesWritten, 5);
  assert.strictEqual(write.buffer, buffer);
  delete write.bytesWritten;
  write.buffer = null;
  const text = await handle.write('!', 5);
  checkRecord(text, ['bytesWritten', 'buffer']);
  assert.strictEqual(text.bytesWritten, 1);
  assert.strictEqual(text.buffer, '!');
  checkStats(await handle.stat(), 6);

  const held = [];
  for (let i = 0; i < 4000; i++) held.push(fs.statSync(file));
  for (const stat of held) checkStats(stat, 6);
  assert.notStrictEqual(held[0].birthtime, held[3999].birthtime);
  const afterChurn = await handle.read(buffer, 0, 5, 0);
  checkRecord(afterChurn, ['bytesRead', 'buffer']);
  assert.strictEqual(afterChurn.bytesRead, 5);
  assert.strictEqual(afterChurn.buffer, buffer);
  assert.strictEqual(nextRead.buffer, buffer);
  const writeAfterChurn = await handle.write(buffer, 0, 5, 0);
  checkRecord(writeAfterChurn, ['bytesWritten', 'buffer']);
  assert.strictEqual(writeAfterChurn.bytesWritten, 5);
  assert.strictEqual(writeAfterChurn.buffer, buffer);

  const cpus = os.cpus();
  for (const cpu of cpus) {
    checkRecord(cpu, ['model', 'speed', 'times']);
    checkRecord(cpu.times, timeKeys);
    assert.strictEqual(typeof cpu.model, 'string');
    assert.strictEqual(typeof cpu.speed, 'number');
    for (const key of timeKeys) assert.strictEqual(typeof cpu.times[key], 'number');
  }
  if (cpus.length) {
    const cpuProto = Object.getPrototypeOf(cpus[0]);
    const original = cpus[0].times.user;
    const heldTimes = cpus[0].times;
    delete cpus[0].model;
    Object.setPrototypeOf(cpus[0], { changed: true });
    cpus[0].times = null;
    for (let i = 0; i < 100; i++) {
      const fresh = os.cpus();
      for (const cpu of fresh) {
        checkRecord(cpu, ['model', 'speed', 'times']);
        checkRecord(cpu.times, timeKeys);
        assert.strictEqual(Object.getPrototypeOf(cpu), cpuProto);
        assert.notStrictEqual(cpu.times, heldTimes);
      }
      if (fresh.length > 1) assert.notStrictEqual(fresh[0].times, fresh[1].times);
    }
    assert.strictEqual(heldTimes.user, original);
  }
}

main().then(() => {
  cleanup();
  console.log('fs/os record templates: ok');
}, err => {
  cleanup();
  console.error(err);
  process.exit(1);
});
