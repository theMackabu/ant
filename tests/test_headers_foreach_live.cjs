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

function sortAndCombine(pairs) {
  const out = [];
  for (const name of [...new Set(pairs.map(([n]) => n))].sort()) {
    const values = pairs.filter(([n]) => n === name).map(([, v]) => v);
    if (name === 'set-cookie') for (const v of values) out.push([name, v]);
    else out.push([name, values.join(', ')]);
  }
  return out;
}

let seed = 7;
const next = (n) => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n;
const names = ['accept', 'set-cookie', 'vary', 'x-a', 'x-b', 'x-c'];
const mutated = new Headers();
let model = [];

for (let step = 0; step < 4000; step++) {
  const name = names[next(names.length)];
  const value = `v${step}`;
  const op = next(4);
  if (op <= 1) {
    mutated.append(name, value);
    model.push([name, value]);
  } else if (op === 2) {
    mutated.set(name, value);
    model = model.filter(([n]) => n !== name);
    model.push([name, value]);
  } else {
    mutated.delete(name);
    model = model.filter(([n]) => n !== name);
  }

  const want = sortAndCombine(model);
  assert.deepStrictEqual([...mutated], want, `step ${step}`);
  assert.deepStrictEqual([...mutated.keys()], want.map(([n]) => n), `keys at step ${step}`);
  const seen = [];
  mutated.forEach((v, n) => seen.push([n, v]));
  assert.deepStrictEqual(seen, want, `forEach at step ${step}`);
}

console.log('headers forEach live ok');
