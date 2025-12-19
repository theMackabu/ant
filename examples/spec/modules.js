import { test, summary } from './helpers.js';

import * as path from 'ant:path';
import * as fs from 'ant:fs';
import * as shell from 'ant:shell';
import * as ffi from 'ant:ffi';

console.log('Module Tests\n');

test('import.meta exists', typeof import.meta, 'object');
test('import.meta.url exists', typeof import.meta.url, 'string');
test('import.meta.url is file', import.meta.url.startsWith('file://'), true);

test('module imported test', typeof test, 'function');
test('module imported summary', typeof summary, 'function');

test('Atomics toStringTag', Object.prototype.toString.call(Atomics), '[object Atomics]');
test('console toStringTag', Object.prototype.toString.call(console), '[object console]');
test('JSON toStringTag', Object.prototype.toString.call(JSON), '[object JSON]');
test('process toStringTag', Object.prototype.toString.call(process), '[object process]');
test('Buffer toStringTag', Object.prototype.toString.call(Buffer), '[object Buffer]');
test('crypto toStringTag', Object.prototype.toString.call(crypto), '[object Crypto]');

test('path toStringTag', Object.prototype.toString.call(path), '[object path]');
test('fs toStringTag', Object.prototype.toString.call(fs), '[object fs]');
test('shell toStringTag', Object.prototype.toString.call(shell), '[object shell]');
test('ffi toStringTag', Object.prototype.toString.call(ffi), '[object FFI]');

summary();
