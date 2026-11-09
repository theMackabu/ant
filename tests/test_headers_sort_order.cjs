// Headers iteration sorts names and keeps same-name values in insertion order
// (Fetch "sort and combine"). qsort isn't stable on every libc (musl reorders
// equal keys), so check many list shapes against the spec algorithm.
const assert = require('node:assert');

function sortAndCombine(pairs) {
  const out = [];
  for (const name of [...new Set(pairs.map(([n]) => n))].sort()) {
    const values = pairs.filter(([n]) => n === name).map(([, v]) => v);
    if (name === 'set-cookie') for (const v of values) out.push([name, v]);
    else out.push([name, values.join(', ')]);
  }
  return out;
}

let seed = 1;
const next = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 2 ** 32;
const names = ['accept', 'set-cookie', 'vary', 'x-a', 'x-b'];

for (let n = 2; n <= 64; n++) {
  for (let round = 0; round < 8; round++) {
    const pairs = [];
    for (let i = 0; i < n; i++) pairs.push([names[Math.floor(next() * names.length)], `v${i}`]);

    const headers = new Headers();
    for (const [name, value] of pairs) headers.append(name, value);

    const want = sortAndCombine(pairs);
    assert.deepStrictEqual([...headers], want, `n=${n} round=${round}`);
    for (const [name, value] of want) if (name !== 'set-cookie') assert.strictEqual(headers.get(name), value);
    assert.deepStrictEqual(headers.getSetCookie(), pairs.filter(([n]) => n === 'set-cookie').map(([, v]) => v));
  }
}

console.log('headers sort order ok');
