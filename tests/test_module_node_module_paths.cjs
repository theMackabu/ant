const assert = require('node:assert');
const path = require('node:path');
const { Module } = require('node:module');

assert.deepStrictEqual(Module._nodeModulePaths('/a/b/node_modules/c'), [
  '/a/b/node_modules/c/node_modules',
  '/a/b/node_modules',
  '/a/node_modules',
  '/node_modules',
]);
assert.deepStrictEqual(Module._nodeModulePaths('/'), ['/node_modules']);
assert.deepStrictEqual(Module._nodeModulePaths('/node_modules'), ['/node_modules']);
assert.deepStrictEqual(Module._nodeModulePaths('/a//b/./c/../d/'), [
  '/a/b/d/node_modules',
  '/a/b/node_modules',
  '/a/node_modules',
  '/node_modules',
]);
assert.deepStrictEqual(Module._nodeModulePaths('rel'), Module._nodeModulePaths(path.resolve('rel')));
assert.throws(() => Module._nodeModulePaths(1), TypeError);

console.log('module node module paths: ok');
