import { test, summary } from './helpers.js';
import fs from 'ant:fs';
import path from 'ant:path';

console.log('FS Tests\n');

const testDir = '/tmp/ant-spec-test';
const testFile = path.join(testDir, 'test.txt');

fs.mkdirSync(testDir, { recursive: true });
test('mkdirSync creates dir', fs.existsSync(testDir), true);

fs.writeFileSync(testFile, 'hello world');
test('writeFileSync creates file', fs.existsSync(testFile), true);

const content = fs.readFileSync(testFile, 'utf8');
test('readFileSync content', content, 'hello world');

fs.appendFileSync(testFile, '!');
const appended = fs.readFileSync(testFile, 'utf8');
test('appendFileSync', appended, 'hello world!');

const stat = fs.statSync(testFile);
test('statSync isFile', stat.isFile(), true);
test('statSync isDirectory', stat.isDirectory(), false);
test('statSync size', stat.size > 0, true);

const dirStat = fs.statSync(testDir);
test('statSync dir isDirectory', dirStat.isDirectory(), true);

const copyFile = path.join(testDir, 'copy.txt');
fs.copyFileSync(testFile, copyFile);
test('copyFileSync', fs.existsSync(copyFile), true);

const renameFile = path.join(testDir, 'renamed.txt');
fs.renameSync(copyFile, renameFile);
test('renameSync', fs.existsSync(renameFile), true);
test('renameSync removes old', fs.existsSync(copyFile), false);

const files = fs.readdirSync(testDir);
test('readdirSync length', files.length >= 2, true);
test('readdirSync includes test.txt', files.includes('test.txt'), true);

fs.unlinkSync(testFile);
test('unlinkSync removes file', fs.existsSync(testFile), false);

fs.unlinkSync(renameFile);

const fdFile = path.join(testDir, 'fd_test.txt');
const fd = fs.openSync(fdFile, 'w');
test('openSync returns fd', typeof fd, 'number');

const written = fs.writeSync(fd, 'hello fd');
test('writeSync returns bytes written', written, 8);

const buf = new Uint8Array([33]);
const bufWritten = fs.writeSync(fd, buf);
test('writeSync buffer returns bytes written', bufWritten, 1);

fs.closeSync(fd);

const fdContent = fs.readFileSync(fdFile, 'utf8');
test('writeSync wrote correct data', fdContent, 'hello fd!');

fs.unlinkSync(fdFile);

const wvFile = path.join(testDir, 'writev_test.txt');
const wvFd = fs.openSync(wvFile, 'w');
const bufs = [new Uint8Array([65, 66]), new Uint8Array([67, 68])];
const wvWritten = fs.writevSync(wvFd, bufs);
test('writevSync returns total bytes', wvWritten, 4);
fs.closeSync(wvFd);

const wvContent = fs.readFileSync(wvFile, 'utf8');
test('writevSync wrote correct data', wvContent, 'ABCD');
fs.unlinkSync(wvFile);

fs.rmdirSync(testDir);
test('rmdirSync removes dir', fs.existsSync(testDir), false);

summary();
