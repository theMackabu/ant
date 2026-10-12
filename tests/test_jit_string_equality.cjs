// compiled === and !== decide short flat strings inline; results must match a character compare
const assert = require('node:assert');

const words = ['', 'a', 'q', 'ab', 'ba', 'abcdefg', 'abcdefh', 'abcdefgh', 'abcdefgi', 'bbcdefgh', 'abcdefghi', 'abcdefghj', 'é', 'è', 'ée', '한국', '😀', 'x'.repeat(40), 'x'.repeat(39) + 'y'];
const copies = words.map(w => w.split('').join(''));
const ropes = words.map(w => (w + 'z'.repeat(40)).slice(0, w.length));
const values = words.concat(copies, ropes, words.map(w => `${w}`));

const reference = (a, b) => {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a.charCodeAt(i) !== b.charCodeAt(i)) return false;
  return true;
};

const strict = (a, b) => a === b;
const strictNot = (a, b) => a !== b;
const literal = s => [s === 'abcdefgh', s !== 'q', 'é' === s];

for (let round = 0; round < 300; round++) {
  for (const a of values) for (const b of values) {
    const want = reference(a, b);
    assert.strictEqual(strict(a, b), want, `${JSON.stringify(a)} === ${JSON.stringify(b)}`);
    assert.strictEqual(strictNot(a, b), !want, `${JSON.stringify(a)} !== ${JSON.stringify(b)}`);
  }
  for (const s of values) assert.deepStrictEqual(literal(s), [reference(s, 'abcdefgh'), !reference(s, 'q'), reference(s, 'é')]);
}

console.log('jit string equality OK');
