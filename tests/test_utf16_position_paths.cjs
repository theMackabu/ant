const assert = require('node:assert');

const pieces = ['hello ', 'café ', '日本語テキスト', '😀🎉', 'Ωμέγα ', 'abc123 ', '\uD800', 'ü', '🚀x'];
let text = '';
for (let i = 0; text.length < 50000; i++) text += pieces[(i * 7) % pieces.length];

const units = [];
for (const cp of text) {
  const code = cp.codePointAt(0);
  if (code > 0xffff) units.push(0xd800 + ((code - 0x10000) >> 10), 0xdc00 + ((code - 0x10000) & 0x3ff));
  else units.push(code);
}
assert.strictEqual(text.length, units.length);

const decoded = Buffer.from(text.replace(/\uD800/g, '')).toString('utf8');
assert.strictEqual(decoded.length, text.replace(/\uD800/g, '').length);

for (let pass = 0; pass < 3; pass++)
  for (let i = 0; i < text.length; i += 997) assert.strictEqual(text.charCodeAt(i), units[i]);

let seed = 1;
const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
for (let k = 0; k < 5000; k++) {
  const i = rnd(text.length);
  assert.strictEqual(text.charCodeAt(i), units[i]);
  const s = text.slice(i, i + 5);
  for (let j = 0; j < s.length; j++) assert.strictEqual(s.charCodeAt(j), units[i + j]);
}

for (let i = text.length - 1; i >= 0; i -= 641) assert.strictEqual(text.charCodeAt(i), units[i]);

const re = /[日本]+語|😀/gu;
let m;
let matches = 0;
while ((m = re.exec(text))) {
  assert.strictEqual(text.slice(m.index, m.index + m[0].length), m[0]);
  assert.strictEqual(re.lastIndex, m.index + m[0].length);
  matches++;
}
assert.ok(matches > 100);

let count = 0;
for (const ch of text) count += ch.length;
assert.strictEqual(count, text.length);

console.log('ok');
