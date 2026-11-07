const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-fs-stats-bigint-'));
const file = path.join(dir, 'file.txt');
const link = path.join(dir, 'link');
fs.writeFileSync(file, 'hello');
fs.symlinkSync(file, link);

const INTEGERS = ['dev', 'ino', 'mode', 'nlink', 'uid', 'gid', 'rdev', 'size', 'blksize', 'blocks'];
const TIMES = ['atime', 'mtime', 'ctime', 'birthtime'];

function checkBigInt(stats, plain, what) {
  for (const key of INTEGERS) {
    assert.strictEqual(typeof stats[key], 'bigint', `${what}.${key}`);
    assert.strictEqual(stats[key], BigInt(plain[key]), `${what}.${key} value`);
  }
  for (const time of TIMES) {
    const ms = stats[`${time}Ms`];
    const ns = stats[`${time}Ns`];
    assert.strictEqual(typeof ms, 'bigint', `${what}.${time}Ms`);
    assert.strictEqual(typeof ns, 'bigint', `${what}.${time}Ns`);
    assert.strictEqual(ms, ns / 1000000n, `${what}.${time}Ms is ${time}Ns truncated`);
    assert.ok(stats[time] instanceof Date, `${what}.${time} is a Date`);
    assert.strictEqual(stats[time].getTime(), Number(ms), `${what}.${time} matches ${time}Ms`);
  }
  assert.strictEqual(stats.size, 5n);
}

function checkPlain(stats, what) {
  for (const key of INTEGERS) assert.strictEqual(typeof stats[key], 'number', `${what}.${key}`);
  for (const time of TIMES) {
    assert.strictEqual(stats[`${time}Ns`], undefined, `${what}.${time}Ns`);
    assert.strictEqual(stats[time].getTime(), Math.round(stats[`${time}Ms`]), `${what}.${time} rounds ${time}Ms`);
  }
}

async function main() {
  const plain = fs.statSync(file);
  checkPlain(plain, 'statSync');
  checkPlain(fs.statSync(file, { bigint: false }), 'statSync bigint:false');

  const big = fs.statSync(file, { bigint: true });
  checkBigInt(big, plain, 'statSync');
  assert.strictEqual(big.isFile(), true);
  assert.strictEqual(big.isDirectory(), false);

  const fd = fs.openSync(file, 'r');
  try {
    checkBigInt(fs.fstatSync(fd, { bigint: true }), plain, 'fstatSync');
    checkPlain(fs.fstatSync(fd), 'fstatSync');
  } finally {
    fs.closeSync(fd);
  }

  const linkStats = fs.lstatSync(link, { bigint: true });
  assert.strictEqual(typeof linkStats.ino, 'bigint');
  assert.strictEqual(linkStats.isSymbolicLink(), true);
  assert.strictEqual(fs.statSync(path.join(dir, 'missing'), { bigint: true, throwIfNoEntry: false }), undefined);

  checkBigInt(await fs.promises.stat(file, { bigint: true }), plain, 'promises.stat');
  checkBigInt(await fs.promises.lstat(file, { bigint: true }), plain, 'promises.lstat');
  checkPlain(await fs.promises.stat(file), 'promises.stat');

  const handle = await fs.promises.open(file);
  try {
    checkBigInt(await handle.stat({ bigint: true }), plain, 'FileHandle.stat');
  } finally {
    await handle.close();
  }

  const viaCallback = await new Promise((resolve, reject) => {
    fs.stat(file, { bigint: true }, (error, stats) => (error ? reject(error) : resolve(stats)));
  });
  checkBigInt(viaCallback, plain, 'stat callback');

  const viaFstat = await new Promise((resolve, reject) => {
    const callbackFd = fs.openSync(file, 'r');
    fs.fstat(callbackFd, { bigint: true }, (error, stats) => {
      fs.closeSync(callbackFd);
      error ? reject(error) : resolve(stats);
    });
  });
  checkBigInt(viaFstat, plain, 'fstat callback');

  console.log('fs bigint stats: ok');
}

main()
  .catch(error => {
    process.exitCode = 1;
    console.error(error);
  })
  .finally(() => fs.rmSync(dir, { recursive: true, force: true }));
