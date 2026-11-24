const assert = require('node:assert');

const compileOnly = Ant.raw.compileOnly;
let ran = 0;
globalThis.__compileOnlyRan = () => ran++;

assert.strictEqual(compileOnly("import fs from 'node:fs'; export const x = await Promise.resolve(1); __compileOnlyRan(); export default class A {}", '/tmp/a.mjs'), undefined);
assert.strictEqual(compileOnly("const x = require('x'); __compileOnlyRan(); if (x) return; module.exports = x;", '/tmp/a.cjs'), undefined);
assert.strictEqual(compileOnly('interface P { a: number } export function f(p: P): number { __compileOnlyRan(); return p.a as number; } enum E { A }', '/tmp/a.ts'), undefined);
assert.strictEqual(compileOnly('export const y = 1; __compileOnlyRan();', '/tmp/ant-compile-only-no-package/a.js'), undefined);
assert.strictEqual(compileOnly('var y = 1; __compileOnlyRan(); return y;', '/tmp/ant-compile-only-no-package/b.js'), undefined);
assert.strictEqual(compileOnly('export const z = 2;'), undefined);
assert.strictEqual(ran, 0);

assert.throws(() => compileOnly('export const = ;', '/tmp/b.mjs'), SyntaxError);
assert.throws(() => compileOnly('const = ;', '/tmp/b.cjs'), SyntaxError);
assert.throws(() => compileOnly('return 1;', '/tmp/c.mjs'), SyntaxError);
assert.throws(() => compileOnly("import a from 'a';", '/tmp/c.cjs'), SyntaxError);
assert.throws(() => compileOnly(42), TypeError);
assert.throws(() => compileOnly(), TypeError);

for (let i = 0; i < 200; i++) compileOnly(`export const v${i} = ${i}; function f${i}(a, b) { return a + b + ${i}; }`, '/tmp/loop.mjs');
assert.strictEqual(ran, 0);
assert.strictEqual(typeof Ant.raw.compileOnly, 'function');

console.log('ok');
