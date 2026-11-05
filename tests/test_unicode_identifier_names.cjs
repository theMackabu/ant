const assert = require('node:assert');

const otherIdStart = [0x1885, 0x1886, 0x2118, 0x212e, 0x309b, 0x309c];
for (const cp of otherIdStart) {
  const ch = String.fromCodePoint(cp);
  assert.strictEqual(new Function(`var ${ch}a = 1; return ${ch}a`)(), 1);
  assert.strictEqual(new Function(`var a${ch} = 2; return a${ch}`)(), 2);
  const esc = `\\u{${cp.toString(16)}}`;
  assert.strictEqual(new Function(`var ${esc}b = 3; return ${ch}b`)(), 3);
  assert.strictEqual(new RegExp(`(?<${ch}>x)`).exec('x').groups[ch], 'x');
}

const otherIdContinue = [0x00b7, 0x0387, 0x1369, 0x1371, 0x19da, 0x30fb, 0xff65];
for (const cp of otherIdContinue) {
  const ch = String.fromCodePoint(cp);
  assert.strictEqual(new Function(`var a${ch} = 4; return a${ch}`)(), 4);
  assert.throws(() => new Function(`var ${ch}a = 1`), SyntaxError);
}

assert.throws(() => new Function('var ⸯ = 1'), SyntaxError);
assert.throws(() => new Function('var aⸯ = 1'), SyntaxError);

console.log('ok');
