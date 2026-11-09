const assert = require('node:assert');

function viaForEach(init, mutate) {
  const headers = new Headers(init);
  const seen = [];
  let calls = 0;
  headers.forEach((value, name) => {
    seen.push([name, value]);
    mutate(headers, name, calls++);
  });
  return seen;
}

function viaIterator(init, mutate) {
  const headers = new Headers(init);
  const seen = [];
  let calls = 0;
  for (const [name, value] of headers) {
    seen.push([name, value]);
    mutate(headers, name, calls++);
  }
  return seen;
}

const cases = [
  {
    name: 'append sorting after the cursor is visited',
    init: { b: '1', c: '2' },
    mutate: (h, name) => { if (name === 'b') h.append('d', '3'); },
    want: [['b', '1'], ['c', '2'], ['d', '3']],
  },
  {
    name: 'append sorting before the cursor revisits the current entry',
    init: { b: '1', c: '2' },
    mutate: (h, name, calls) => { if (calls === 0) h.append('a', '0'); },
    want: [['b', '1'], ['b', '1'], ['c', '2']],
  },
  {
    name: 'deleting a later name skips it',
    init: { a: '1', b: '2', c: '3' },
    mutate: (h) => h.delete('c'),
    want: [['a', '1'], ['b', '2']],
  },
  {
    name: 'deleting the current name skips the next one',
    init: { a: '1', b: '2', c: '3' },
    mutate: (h, name) => h.delete(name),
    want: [['a', '1'], ['c', '3']],
  },
  {
    name: 'set on a later name is observed',
    init: { a: '1', b: '2' },
    mutate: (h) => h.set('b', 'x'),
    want: [['a', '1'], ['b', 'x']],
  },
  {
    name: 'append to a later name is combined',
    init: { a: '1', b: '2' },
    mutate: (h, name, calls) => { if (calls === 0) h.append('b', '9'); },
    want: [['a', '1'], ['b', '2, 9']],
  },
  {
    name: 'nested iteration does not disturb the outer one',
    init: { a: '1', b: '2', c: '3' },
    mutate: (h) => assert.deepStrictEqual([...h.keys()], ['a', 'b', 'c']),
    want: [['a', '1'], ['b', '2'], ['c', '3']],
  },
];

for (const { name, init, mutate, want } of cases) {
  assert.deepStrictEqual(viaForEach(init, mutate), want, `forEach: ${name}`);
  assert.deepStrictEqual(viaIterator(init, mutate), want, `iterator: ${name}`);
}

const headers = new Headers({ a: '1', b: '2' });
assert.throws(() => headers.forEach(() => { throw new RangeError('stop'); }), RangeError);
assert.deepStrictEqual([...headers], [['a', '1'], ['b', '2']]);

console.log('headers forEach live ok');
