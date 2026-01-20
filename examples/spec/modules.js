import { test, summary } from './helpers.js';

import * as path from 'ant:path';
import * as fs from 'ant:fs';
import * as shell from 'ant:shell';
import * as ffi from 'ant:ffi';

import testJson from './test.json';
import { name, version, count } from './test.json';
import textContent from './test.txt';

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
test('Buffer toStringTag', Object.prototype.toString.call(Buffer.alloc(0)), '[object Buffer]');
test('crypto toStringTag', Object.prototype.toString.call(crypto), '[object Crypto]');

test('path toStringTag', Object.prototype.toString.call(path), '[object path]');
test('fs toStringTag', Object.prototype.toString.call(fs), '[object fs]');
test('shell toStringTag', Object.prototype.toString.call(shell), '[object shell]');
test('ffi toStringTag', Object.prototype.toString.call(ffi), '[object FFI]');

test('JSON default import', typeof testJson, 'object');
test('JSON default import name', testJson.name, 'test-package');
test('JSON default import version', testJson.version, '1.0.0');
test('JSON default import count', testJson.count, 42);
test('JSON named import name', name, 'test-package');
test('JSON named import version', version, '1.0.0');
test('JSON named import count', count, 42);

test('text default import type', typeof textContent, 'string');
test('text default import value', textContent, 'Hello from text file\n');

summary();
