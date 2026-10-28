const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const hi = '\ud83d', lo = '\ude00';
const fillers = ['', 'a', '한', '힣국', 'x'.repeat(31), '텍'.repeat(11), 'é'.repeat(16)];
const tails = [hi, lo, lo + hi, hi + 'z', 'z' + lo + '한', '😀', '한국어 텍스트 힣', 'plain'];

const cases = [];
for (const fill of fillers)
  for (let pad = 0; pad < 40; pad += 7)
    for (const tail of tails) cases.push(fill + '.'.repeat(pad) + tail + fill);

const toBytes = (s) => Buffer.from(s, 'utf8').toString('hex');
const isWellFormed = (s) => {
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0xd800 || c > 0xdfff) continue;
    const d = s.charCodeAt(i + 1);
    if (c > 0xdbff || !(d >= 0xdc00 && d <= 0xdfff)) return false;
    i++;
  }
  return true;
};

if (process.argv[2] === 'child') {
  const mode = process.argv[3];
  for (const s of cases) {
    if (mode === 'write') process.stdout.write(s + '\n');
    else console.log(s);
  }
  process.exit(0);
}

for (const mode of ['write', 'log']) {
  const out = spawnSync(process.execPath, [__filename, 'child', mode]);
  assert.strictEqual(out.status, 0, String(out.stderr));
  const expected = Buffer.from(cases.map((s) => s + '\n').join(''), 'utf8');
  assert.strictEqual(out.stdout.toString('hex'), expected.toString('hex'), mode);
}

for (const s of cases) {
  assert.strictEqual(s.isWellFormed(), isWellFormed(s), JSON.stringify(s));
  assert.strictEqual(toBytes(s.toWellFormed()), toBytes(s), JSON.stringify(s));
}

const big = '한국어 텍스트 힣 '.repeat(5000);
assert.strictEqual(big.isWellFormed(), true);
assert.strictEqual((big + hi).isWellFormed(), false);
assert.strictEqual((hi + big).toWellFormed(), '\ufffd' + big);

console.log('ok');
