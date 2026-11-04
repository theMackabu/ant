import assert from 'node:assert';

const strings = [
  ['3', 3], ['007', 7], ['', 0], [' 12 ', 12], ['\t5\n', 5], ['0x10', 16], ['0b11', 3], ['0o17', 15],
  ['1e3', 1000], ['-0', -0], ['-12', -12], ['+4', 4], ['1.5', 1.5], ['abc', NaN], ['12a', NaN],
  ['999999999999999', 999999999999999], ['9999999999999999', 1e16], ['12345678901234567890', 12345678901234567000],
  ['Infinity', Infinity], ['-Infinity', -Infinity], [' 9', 9], ['１', NaN],
];

let valueOfCalls = 0;
const boxed = { valueOf() { valueOfCalls++; return 41; } };
const others = [[true, 1], [false, 0], [null, 0], [undefined, NaN], [boxed, 41], [[], 0], [[7], 7], [new Number(3), 3]];

function plus(x) { return +x; }
function plusAdd(x, i) { return +x + i; }
function square(x) { return x ** 2; }
function power(x, y) { return x ** y; }

function same(actual, expected, label) {
  assert.ok(Object.is(actual, expected), `${label}: ${actual} !== ${expected}`);
}

for (let round = 0; round < 3000; round++) {
  for (const [input, expected] of strings) same(plus(input), expected, JSON.stringify(input));
  for (const [input, expected] of others) same(plus(input), expected, String(input));
  same(plusAdd('20', round), 20 + round, 'plusAdd');
  same(square(round), round * round, 'square');
  same(square(-0), 0, 'square -0');
  same(square(NaN), NaN, 'square NaN');
  same(square(-Infinity), Infinity, 'square -Infinity');
  same(square(1e200), Infinity, 'square overflow');
  same(square(1.1), 1.1 * 1.1, 'square 1.1');
  same(power(2, 10), 1024, 'power');
  same(power(1, Infinity), NaN, 'power 1 Infinity');
  same(power(NaN, 0), 1, 'power NaN 0');
  same(power('3', 2), 9, 'power string');
  same(power(boxed, 2), 1681, 'power object');
}
assert.strictEqual(valueOfCalls, 6000);

for (const bad of [Symbol('s'), 1n]) {
  assert.throws(() => plus(bad), TypeError);
  assert.throws(() => +bad, TypeError);
}
assert.strictEqual(2n ** 3n, 8n);
assert.throws(() => power(2n, 3), TypeError);

let total = 0;
const limit = +'300000';
for (let i = 0; i < limit; i++) total += +'2' + (i % 7) ** 2;
assert.strictEqual(total, 600000 + 42857 * 91);

let thrown = 0;
for (let i = 0; i < 20000; i++) {
  try {
    total += +(i === 19999 ? Symbol('late') : '1');
  } catch (err) {
    assert.ok(err instanceof TypeError);
    thrown++;
  }
}
assert.strictEqual(thrown, 1);

console.log('ok');
