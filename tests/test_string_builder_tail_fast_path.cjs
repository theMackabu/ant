const assert = require('node:assert');

function joined(parts) {
  return parts.map(String).join('');
}

function appendAll(parts) {
  let s = '';
  for (let i = 0; i < parts.length; i++) s += parts[i];
  return s;
}

function appendAllParam(s, parts) {
  for (let i = 0; i < parts.length; i++) s += parts[i];
  return s;
}

function check(name, parts) {
  const want = joined(parts);
  for (let round = 0; round < 3; round++) {
    const got = appendAll(parts);
    assert.strictEqual(got.length, want.length, `${name}: length`);
    assert.strictEqual(got, want, `${name}: content`);
    const viaParam = appendAllParam('', parts);
    assert.strictEqual(viaParam, want, `${name}: param slot`);
    const prefixed = appendAllParam('pé', parts);
    assert.strictEqual(prefixed, 'pé' + want, `${name}: non-ascii seed`);
  }
}

function repeat(value, count) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(value);
  return out;
}

check('empty pieces', ['a', '', 'b', '', '', 'c', '']);
check('only empty', repeat('', 300));
check('two byte ascii', repeat('ab', 5000));

for (const size of [1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128, 255, 256, 257, 300]) {
  const piece = 'x'.repeat(size - 1) + 'y';
  check(`piece size ${size}`, repeat(piece, 40));
  check(`piece size ${size} after one`, ['q'].concat(repeat(piece, 40)));
}

for (const fill of [253, 254, 255, 256]) {
  const head = 'h'.repeat(fill);
  check(`tail fill ${fill} then one`, ['a', 'b', head.slice(2), 'z', 'w']);
  check(`tail fill ${fill} then two`, ['a', 'b', head.slice(2), 'zz', 'w']);
  check(`tail fill ${fill} then number`, ['a', 'b', head.slice(2), 7, 'w']);
  check(`tail fill ${fill} then wide number`, ['a', 'b', head.slice(2), -2147483648, 'w']);
  check(`tail fill ${fill} then empty`, ['a', 'b', head.slice(2), '', 'w']);
}

check('exact tail steps', repeat('abcdefgh', 32 * 9));
check('exact tail by 16', repeat('0123456789abcdef', 16 * 5));

const ints = [];
for (let i = 0; i < 3000; i++) ints.push('ab', i);
check('string and index', ints);

check('integer widths', [
  's', 0, 9, 10, 99, 100, 999, 1000, 9999, 10000, 99999, 100000, 999999, 1000000,
  9999999, 10000000, 99999999, 100000000, 999999999, 1000000000, 2147483647,
  -1, -9, -10, -99, -100, -2147483647, -2147483648,
]);
check('non int32 numbers', [
  's', 2147483648, -2147483649, 4294967296, 0.5, -0.5, 1e21, 1e-7, 123456789012345680000,
  NaN, Infinity, -Infinity, -0, 0, 1.25, 2 ** 53, -(2 ** 53),
]);
check('mostly numbers', (() => {
  const out = ['n'];
  for (let i = -400; i < 400; i++) out.push(i * 7919);
  return out;
})());

check('latin1', repeat('café', 400));
check('ascii then latin1', ['ab', 'cd', 'é', 'ef', 'üü', 'gh']);
check('three byte', repeat('€中', 300));
check('astral', repeat('😀', 300));
check('ascii and astral mix', (() => {
  const out = [];
  for (let i = 0; i < 600; i++) out.push(i % 5 === 0 ? '😀' : 'ok', i);
  return out;
})());

{
  let s = '';
  const want = [];
  for (let i = 0; i < 500; i++) {
    s += 'x';
    s += '\ud83d';
    s += '\ude00';
    want.push('x😀');
  }
  const expected = want.join('');
  assert.strictEqual(s.length, expected.length, 'split surrogate length');
  assert.strictEqual(s, expected, 'split surrogate content');
  assert.strictEqual(s.codePointAt(1), 0x1f600, 'split surrogate joins');
}

{
  let s = '';
  for (let i = 0; i < 100; i++) {
    s += '\ud83d';
    s += 'a';
    s += '\ude00';
  }
  assert.strictEqual(s.length, 300, 'lone surrogate length');
  assert.strictEqual(s.charCodeAt(0), 0xd83d, 'lone high stays');
  assert.strictEqual(s.charCodeAt(2), 0xde00, 'lone low stays');
}

{
  let s = '';
  const snaps = [];
  for (let i = 0; i < 2000; i++) {
    s += 'ab';
    if (i % 97 === 0) snaps.push([i, s]);
    s += i;
  }
  let ref = '';
  let at = 0;
  for (let i = 0; i < 2000; i++) {
    ref += 'ab';
    if (i % 97 === 0) {
      assert.strictEqual(snaps[at][0], i);
      assert.strictEqual(snaps[at][1].length, ref.length, `snapshot ${i} length`);
      assert.strictEqual(snaps[at][1], ref.slice(0), `snapshot ${i} content`);
      at++;
    }
    ref += String(i);
  }
  assert.strictEqual(s, ref, 'builder after snapshots');
}

{
  let s = '';
  for (let i = 0; i < 300; i++) s += 'k';
  const before = s;
  const holder = { v: s };
  s += 'tail';
  s += 5;
  assert.strictEqual(before.length, 300, 'snapshot keeps length');
  assert.strictEqual(holder.v, 'k'.repeat(300), 'snapshot keeps content');
  assert.strictEqual(s, 'k'.repeat(300) + 'tail5', 'builder continues');
}

{
  let s = 'ab';
  for (let i = 0; i < 14; i++) s += s;
  assert.strictEqual(s.length, 2 << 14, 'self append length');
  assert.strictEqual(s.slice(0, 6), 'ababab', 'self append prefix');
  assert.strictEqual(s.slice(-4), 'abab', 'self append suffix');
  let t = '';
  for (let i = 0; i < 40; i++) { t += 'xy'; t += i; }
  const once = t;
  t += t;
  assert.strictEqual(t, once + once, 'self append after tail writes');
  t += 'z';
  assert.strictEqual(t, once + once + 'z', 'append after self append');
}

{
  const big = 'L'.repeat(100000);
  check('very long', ['a', big, 'b', big, 3, big]);
  let s = '';
  for (let i = 0; i < 50; i++) { s += 'ab'; s += big; s += i; }
  assert.strictEqual(s.length, 50 * (2 + 100000) + 10 + 40 * 2, 'long mixed length');
  assert.strictEqual(s.slice(0, 5), 'abLLL', 'long mixed prefix');
  assert.strictEqual(s.slice(-3), 'L49', 'long mixed suffix');
}

{
  let s = '';
  for (let i = 0; i < 1000; i++) {
    s += 'ab';
    s += i;
    assert.strictEqual(s.length, 2 * (i + 1) + (i < 10 ? i + 1 : i < 100 ? 10 + (i - 9) * 2 : 190 + (i - 99) * 3), `length at ${i}`);
  }
  assert.strictEqual(s.slice(0, 12), 'ab0ab1ab2ab3');
  assert.strictEqual(s.charCodeAt(s.length - 1), 57);
}

{
  let s = '';
  for (let i = 0; i < 400; i++) {
    s += 'ab';
    s += 'é';
    s += i;
  }
  let ref = '';
  for (let i = 0; i < 400; i++) ref = ref + 'ab' + 'é' + String(i);
  assert.strictEqual(s.length, ref.length, 'non-ascii interleaved length');
  assert.strictEqual(s, ref, 'non-ascii interleaved content');
}

{
  const objects = [{ toString() { return 'T'; } }, [1, 2], null, undefined, true, 12n, { valueOf() { return 4; } }];
  let s = '';
  let ref = [];
  for (let i = 0; i < 300; i++) {
    const v = objects[i % objects.length];
    s += 'ab';
    s += v;
    ref.push('ab', '' + v);
  }
  assert.strictEqual(s, ref.join(''), 'mixed operand kinds');
  assert.throws(() => { let q = ''; for (let i = 0; i < 5; i++) { q += 'ab'; q += Symbol('x'); } }, TypeError);
}

{
  function outer() {
    let s = '';
    function add(v) { s += v; }
    for (let i = 0; i < 2000; i++) { add('ab'); add(i); }
    return s;
  }
  const got = outer();
  let ref = [];
  for (let i = 0; i < 2000; i++) ref.push('ab', i);
  assert.strictEqual(got, ref.join(''), 'captured builder');
}

{
  let n = 0;
  for (let i = 0; i < 2000; i++) n += i;
  assert.strictEqual(n, 1999000, 'numeric append slot');
  let m = 5;
  for (let i = 0; i < 300; i++) m += i < 150 ? 1 : 'x';
  assert.strictEqual(m, '155' + 'x'.repeat(150), 'number then string');
}

console.log('ok');
