// compiled === and !== compare strings inline (short, long, flattened ropes, literals); results must match a character compare
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

// lengths past one word compare word by word and then a last word that overlaps the end
const alphabet = 'abcdefghijklmnopqrstuvwxyz';
const longCases = [];
for (let len = 9; len <= 20; len++) {
  const base = alphabet.slice(0, len);
  longCases.push([base, base.split('').join(''), true]);
  for (let p = 0; p < len; p++) longCases.push([base, base.slice(0, p) + 'Z' + base.slice(p + 1), false]);
}

const ropeOf = (head, tail) => head + tail;
const unflattened = ropeOf('r'.repeat(20), 's'.repeat(20));
const flattened = ropeOf('r'.repeat(20), 's'.repeat(20));
flattened.charCodeAt(0);
longCases.push([unflattened, 'r'.repeat(20) + 's'.repeat(20), true], [flattened, 'r'.repeat(20) + 's'.repeat(20), true]);
longCases.push([flattened, 'r'.repeat(20) + 's'.repeat(19) + 't', false], [unflattened, flattened, true]);

for (let round = 0; round < 300; round++)
  for (const [a, b, want] of longCases) {
    assert.strictEqual(strict(a, b), want, `${JSON.stringify(a)} === ${JSON.stringify(b)}`);
    assert.strictEqual(strictNot(b, a), !want, `${JSON.stringify(b)} !== ${JSON.stringify(a)}`);
  }

// a literal operand compares length and bytes against immediates, up to a length cap
const literalLens = [0, 1, 2, 7, 8, 9, 15, 16, 17, 20, 63, 64, 65];
const literals = literalLens.map(n => 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+/!'.slice(0, n));
const quoted = literals.map(l => JSON.stringify(l));
const eqRight = new Function('s', `return [${quoted.map(q => `s === ${q}`).join(', ')}];`);
const neLeft = new Function('s', `return [${quoted.map(q => `${q} !== s`).join(', ')}];`);
const viaSwitch = new Function('s', `switch (s) { ${quoted.map((q, i) => `case ${q}: return ${i};`).join(' ')} default: return -1; }`);

const probes = [undefined, null, 0, 1.5, true, {}, [], Symbol.iterator, 'é', 'ée', '😀'];
for (const lit of literals) {
  probes.push(lit, lit.split('').join(''), (lit + 'z'.repeat(40)).slice(0, lit.length));
  const rope = lit.slice(0, lit.length >> 1) + lit.slice(lit.length >> 1);
  probes.push(rope);
  const flat = lit.slice(0, lit.length >> 1) + lit.slice(lit.length >> 1);
  flat.charCodeAt(0);
  probes.push(flat, lit + 'x', lit.slice(0, -1));
  for (let p = 0; p < lit.length; p++) probes.push(lit.slice(0, p) + '#' + lit.slice(p + 1));
}

for (let round = 0; round < 200; round++)
  for (const s of probes) {
    const want = literals.map(l => typeof s === 'string' && reference(s, l));
    assert.deepStrictEqual(eqRight(s), want, `${String(s)} === literals`);
    assert.deepStrictEqual(neLeft(s), want.map(w => !w), `literals !== ${String(s)}`);
    assert.strictEqual(viaSwitch(s), want.indexOf(true), `switch on ${String(s)}`);
  }

const negLiteral = x => (!'abc') === x;
const notNotNull = x => (!!null) === x;
const isUndefLiteral = x => (undefined === 'abc') === x;
for (let i = 0; i < 3000; i++) assert.deepStrictEqual([negLiteral('abc'), negLiteral(false), notNotNull(null), notNotNull(false), isUndefLiteral('abc'), isUndefLiteral(false)], [false, true, false, true, false, true], 'an operator result does not keep its operand as a known constant');

console.log('jit string equality OK');
