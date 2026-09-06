import { test, summary } from './helpers.js';

import * as path from 'ant:path';
import * as fs from 'ant:fs';
import * as shell from 'ant:shell';
import * as ffi from 'ant:ffi';
import { createRequire, Module } from 'node:module';

import testJson from './test.json';
import { name, version, count } from './test.json';
import textContent from './test.txt';

console.log('Module Tests\n');

const constructedParent = new Module('parent.cjs');
const constructedChild = new Module('child.cjs', constructedParent);
test('Module constructor name', Module.name, 'Module');
test('new Module creates a Module instance', constructedChild instanceof Module, true);
test('new Module uses Module.prototype', Object.getPrototypeOf(constructedChild) === Module.prototype, true);
test('Module prototype constructor backlink', Module.prototype.constructor === Module, true);
test('new Module preserves id', constructedChild.id, 'child.cjs');
test('new Module starts without filename', constructedChild.filename, null);
test('new Module starts unloaded', constructedChild.loaded, false);
test('new Module starts with empty exports', Object.keys(constructedChild.exports).length, 0);
test('new Module exports inherit Object.prototype', Object.getPrototypeOf(constructedChild.exports) === Object.prototype, true);
test('new Module preserves parent', constructedChild.parent === constructedParent, true);
test('new Module registers with parent', constructedParent.children[0] === constructedChild, true);
test('new Module registers once with parent', constructedParent.children.length, 1);
test('new Module starts without children', constructedChild.children.length, 0);
test('new Module exposes require', typeof constructedChild.require, 'function');

const requireForCache = createRequire(import.meta.url);
test('createRequire shares Module._cache', requireForCache.cache === Module._cache, true);
test('require.cache has null prototype', Object.getPrototypeOf(requireForCache.cache), null);
const realFsForCache = requireForCache('node:fs');
const fakeFsForCache = {};
try {
  requireForCache.cache.fs = { exports: fakeFsForCache };
  test('require.cache overrides bare builtins', requireForCache('fs') === fakeFsForCache, true);
  test('node prefix bypasses require.cache', requireForCache('node:fs') === realFsForCache, true);
} finally {
  delete requireForCache.cache.fs;
}


test('import.meta exists', typeof import.meta, 'object');
test('import.meta.url exists', typeof import.meta.url, 'string');
test('import.meta.url is file', import.meta.url.startsWith('file:'), true);
test('import.meta.url points to modules.js', /\/modules\.js$/.test(import.meta.url), true);

test('module imported test', typeof test, 'function');
test('module imported summary', typeof summary, 'function');

test('Atomics toStringTag', Object.prototype.toString.call(Atomics), '[object Atomics]');
test('console toStringTag', Object.prototype.toString.call(console), '[object console]');
test('JSON toStringTag', Object.prototype.toString.call(JSON), '[object JSON]');
test('process toStringTag', Object.prototype.toString.call(process), '[object process]');
test('Buffer toStringTag', Object.prototype.toString.call(Buffer.alloc(0)), '[object Buffer]');
test('crypto toStringTag', Object.prototype.toString.call(crypto), '[object Crypto]');

test('path namespace toStringTag', Object.prototype.toString.call(path), '[object Module]');
test('path default is an ordinary object', Object.prototype.toString.call(path.default), '[object Object]');
test('fs toStringTag', Object.prototype.toString.call(fs), '[object fs]');
test('shell toStringTag', Object.prototype.toString.call(shell), '[object Module]');
test('shell namespace tag', shell[Symbol.toStringTag], 'Module');
const shellTagDescriptor = Object.getOwnPropertyDescriptor(shell, Symbol.toStringTag);
test('shell namespace tag writable', shellTagDescriptor.writable, false);
test('shell namespace tag enumerable', shellTagDescriptor.enumerable, false);
test('shell namespace tag configurable', shellTagDescriptor.configurable, false);
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

const absTargetUrl =
  typeof import.meta.resolve === 'function'
    ? import.meta.resolve('./import_abs_target.js')
    : import.meta.url.replace(/\/modules\.js$/, '/import_abs_target.js');

const absTargetLooksRight = /\/import_abs_target\.js$/.test(absTargetUrl);
test('absolute file URL target resolves to fixture', absTargetLooksRight, true);

if (absTargetLooksRight) {
  try {
    const absMod = await import(absTargetUrl);
    test('dynamic import absolute file URL marker', absMod.marker, 'abs-import-ok');
  } catch (e) {
    const msg = String(e && (e.stack || e.message || e));
    test('dynamic import absolute file URL works', false, true);
    test('dynamic import absolute file URL error message', typeof msg, 'string');
  }
} else test('dynamic import absolute file URL works', false, true);

summary();
