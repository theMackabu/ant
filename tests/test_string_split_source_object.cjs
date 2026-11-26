const assert = require('node:assert');

function check(name, got, want) {
  for (const p of got) {
    if (typeof p !== 'string') continue;
    assert.ok(p.isWellFormed(), name + ': ill-formed piece ' + JSON.stringify(p));
    assert.strictEqual(JSON.parse(JSON.stringify(p)), p, name + ': JSON round trip');
  }
  assert.deepStrictEqual(got, want, name);
}

const sub = '\u{1F4C1}a\u{1F4C1}b';
check('source empty', sub.split({ source: '' }), [sub]);
check('source empty astral only', '\u{1F4C1}'.split({ source: '' }), ['\u{1F4C1}']);
check('source b', 'abcb'.split({ source: 'b' }), ['abcb']);
check('source dot', 'a.b'.split({ source: '.' }), ['a.b']);
check('object sep', 'a[object Object]b'.split({ source: 'x' }), ['a', 'b']);
check('object sep astral', ('\u{1F4C1}[object Object]\u{1F4C1}').split({ source: '' }), ['\u{1F4C1}', '\u{1F4C1}']);
check('limit', 'a[object Object]b[object Object]c'.split({ source: '' }, 2), ['a', 'b']);
check('limit 0', 'abc'.split({ source: '' }, 0), []);
check('toString', 'a-b-c'.split({ toString() { return '-'; } }), ['a', 'b', 'c']);
check('toString astral', '\u{1F4C1}-\u{1F4C1}'.split({ toString() { return '-'; } }), ['\u{1F4C1}', '\u{1F4C1}']);
check('Symbol.split', 'abc'.split({ [Symbol.split](s, l) { return [s, l]; } }, 7), ['abc', 7]);
check('Symbol.split source', 'abc'.split({ source: 'b', [Symbol.split]() { return ['x']; } }), ['x']);
check('null sep', 'anullb'.split(null), ['a', 'b']);
check('undefined sep', 'ab'.split(undefined), ['ab']);
assert.throws(() => 'ab'.split(Symbol()), TypeError);
{
  let calls = 0;
  check('limit 0 still converts sep', 'a,b'.split({ toString() { calls++; return ','; } }, 0), []);
  assert.strictEqual(calls, 1);
}

const re = /b/;
Object.defineProperty(re, Symbol.split, { value: undefined });
check('regexp without @@split', 'a/b/c/b/'.split(re), ['a', 'c', '']);
check('regexp without @@split no match', 'abc'.split(re), ['abc']);
const re2 = /(?:)/u;
Object.defineProperty(re2, Symbol.split, { value: undefined });
check('empty regexp without @@split', '\u{1F4C1}'.split(re2), ['\u{1F4C1}']);
check('regexp normal', '\u{1F4C1}b\u{1F4C1}'.split(/b/), ['\u{1F4C1}', '\u{1F4C1}']);
check('regexp empty u', '\u{1F4C1}x'.split(/(?:)/u), ['\u{1F4C1}', 'x']);

{
  let calls = 0;
  const obj = { toString() { calls++; return 'x'; } };
  const sp = String.prototype.split;
  assert.deepStrictEqual(sp.call(obj, { [Symbol.split](s) { return [typeof s]; } }), ['object']);
  assert.strictEqual(sp.call(obj, { [Symbol.split](s) { return [s === obj]; } }, 3)[0], true);
  assert.strictEqual(calls, 0);
  assert.deepStrictEqual(sp.call(obj, 'x'), ['', '']);
  assert.strictEqual(calls, 1);
  assert.deepStrictEqual(sp.call(5, { [Symbol.split](s) { return [typeof s]; } }), ['number']);
  assert.deepStrictEqual(sp.call(Symbol(), { [Symbol.split](s) { return [typeof s]; } }), ['symbol']);
  assert.throws(() => sp.call(Symbol(), ','), TypeError);
  for (const bad of [null, undefined]) {
    assert.throws(() => sp.call(bad, { [Symbol.split]() { return []; } }), TypeError);
    assert.throws(() => sp.call(bad, ','), TypeError);
  }
}

const out = '\u{1F4C1}'.split({ source: '' }).join('');
assert.strictEqual(out.replace(/./gu, 'z'), 'z');
console.log('ok');
