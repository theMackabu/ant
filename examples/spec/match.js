import { test, summary } from './helpers.js';

console.log('Ant.match Tests\n');

test('match is function', typeof Ant.match, 'function');

test('exact match number', Ant.match(200, {
  200: 'ok',
  404: 'not found',
}), 'ok');

test('exact match string', Ant.match('hello', {
  hello: 'world',
  foo: 'bar',
}), 'world');

test('no match returns undefined', Ant.match(999, {
  200: 'ok',
  404: 'not found',
}), undefined);

test('Symbol.default fallback', Ant.match(999, {
  200: 'ok',
  [Symbol.default]: 'fallback',
}), 'fallback');

test('Symbol.default as function', Ant.match(999, {
  200: 'ok',
  [Symbol.default]: v => `unknown ${v}`,
}), 'unknown 999');

test('function arm called with value', Ant.match(200, {
  200: v => `got ${v}`,
}), 'got 200');

test('guard arm true', Ant.match(503, {
  200: 'ok',
  [503 >= 500]: 'server error',
}), 'server error');

test('guard arm false skipped', Ant.match(503, {
  [503 < 400]: 'client error',
  [Symbol.default]: 'fallback',
}), 'fallback');

test('exact match takes priority over guard', Ant.match(200, {
  [true]: 'guard',
  200: 'exact',
}), 'exact');

test('arrow function form with $', Ant.match(503, $ => ({
  200: 'ok',
  404: 'not found',
  [$ >= 500]: 'server error',
})), 'server error');

test('arrow $ guard false', Ant.match(200, $ => ({
  [$ >= 500]: 'server error',
  [Symbol.default]: 'other',
})), 'other');

test('compiler sugar with $', Ant.match(404, {
  200: 'ok',
  [$ === 404]: 'not found',
  [Symbol.default]: 'unknown',
}), 'not found');

test('compiler sugar $ receives value', Ant.match(42, {
  [$ > 100]: 'big',
  [$ > 0]: 'positive',
  [Symbol.default]: 'other',
}), 'positive');

test('plain values not called', Ant.match(1, {
  1: 'one',
  2: 'two',
}), 'one');

test('mixed plain and function arms', Ant.match(2, {
  1: 'one',
  2: v => `two-${v}`,
}), 'two-2');

test('empty arms returns undefined', Ant.match(1, {}), undefined);

test('Symbol.default function receives value', Ant.match(42, {
  [Symbol.default]: v => v * 2,
}), 84);

summary();
