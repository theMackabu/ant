import { test, summary } from './helpers.js';
import fs from 'ant:fs';
import path from 'ant:path';

console.log('FS Async Tests\n');

async function run() {
  const testDir = '/tmp/ant-spec-test-async';
  const testFile = path.join(testDir, 'test.txt');

  await fs.mkdir(testDir);
  test('mkdir creates dir', await fs.exists(testDir), true);

  await fs.writeFile(testFile, 'hello world');
  test('writeFile creates file', await fs.exists(testFile), true);

  const content = await fs.readFile(testFile, 'utf8');
  test('readFile content', content, 'hello world');

  const stat = await fs.stat(testFile);
  test('stat isFile', stat.isFile(), true);
  test('stat isDirectory', stat.isDirectory(), false);
  test('stat size', stat.size > 0, true);

  const dirStat = await fs.stat(testDir);
  test('stat dir isDirectory', dirStat.isDirectory(), true);

  const files = await fs.readdir(testDir);
  test('readdir length', files.length >= 1, true);
  test('readdir includes test.txt', files.includes('test.txt'), true);

  await fs.unlink(testFile);
  test('unlink removes file', await fs.exists(testFile), false);

  // fd-based open/write/close
  const fdFile = path.join(testDir, 'fd_test.txt');
  const fd = await fs.open(fdFile, 'w');
  test('open returns fd', typeof fd, 'number');

  const written = await fs.write(fd, 'hello fd');
  test('write returns bytes written', written, 8);

  const buf = new Uint8Array([33]);
  const bufWritten = await fs.write(fd, buf);
  test('write buffer returns bytes written', bufWritten, 1);

  await fs.close(fd);

  const fdContent = fs.readFileSync(fdFile, 'utf8');
  test('write wrote correct data', fdContent, 'hello fd!');

  await fs.unlink(fdFile);

  // writev
  const wvFile = path.join(testDir, 'writev_test.txt');
  const wvFd = await fs.open(wvFile, 'w');
  const bufs = [new Uint8Array([65, 66]), new Uint8Array([67, 68])];
  const wvWritten = await fs.writev(wvFd, bufs);
  test('writev returns total bytes', wvWritten, 4);
  await fs.close(wvFd);

  const wvContent = fs.readFileSync(wvFile, 'utf8');
  test('writev wrote correct data', wvContent, 'ABCD');
  await fs.unlink(wvFile);

  await fs.rmdir(testDir);
  test('rmdir removes dir', await fs.exists(testDir), false);

  summary();
}

void run();
