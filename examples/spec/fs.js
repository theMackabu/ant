import { test, summary } from './helpers.js';
import fs from 'fs';
import path from 'path';

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
fs.rmdirSync(testDir);
test('rmdirSync removes dir', fs.existsSync(testDir), false);

summary();
