const obj = { a: 1, b: 1, c: 1 };
const funcs = [];

for (let key in obj) {
  funcs.push(() => key);
}

const got = funcs.map(fn => fn());
console.log('for-in let closure:', got.join(' '));

const expected = ['a', 'b', 'c'];
if (got.length !== expected.length) {
  throw new Error(`Expected ${expected.length} closures, got ${got.length}`);
}

for (let i = 0; i < expected.length; i++) {
  if (got[i] !== expected[i]) {
    throw new Error(`Expected ${expected.join(' ')}, got ${got.join(' ')}`);
  }
}

console.log('PASS');
