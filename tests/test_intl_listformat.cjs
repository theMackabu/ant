// Intl.ListFormat: constructor validation, format/formatToParts, resolvedOptions.
// Expected values were obtained from Node.
const assert = require('node:assert');

const items = ['a', 'b', 'c', 'd'];
const expected = {
  'conjunction long': ['', 'a', 'a and b', 'a, b, and c', 'a, b, c, and d'],
  'conjunction short': ['', 'a', 'a & b', 'a, b, & c', 'a, b, c, & d'],
  'conjunction narrow': ['', 'a', 'a, b', 'a, b, c', 'a, b, c, d'],
  'disjunction long': ['', 'a', 'a or b', 'a, b, or c', 'a, b, c, or d'],
  'disjunction short': ['', 'a', 'a or b', 'a, b, or c', 'a, b, c, or d'],
  'disjunction narrow': ['', 'a', 'a or b', 'a, b, or c', 'a, b, c, or d'],
  'unit long': ['', 'a', 'a, b', 'a, b, c', 'a, b, c, d'],
  'unit short': ['', 'a', 'a, b', 'a, b, c', 'a, b, c, d'],
  'unit narrow': ['', 'a', 'a b', 'a b c', 'a b c d'],
};
for (const [key, outs] of Object.entries(expected)) {
  const [type, style] = key.split(' ');
  const lf = new Intl.ListFormat('en', { type, style });
  for (let n = 0; n <= 4; n++) assert.strictEqual(lf.format(items.slice(0, n)), outs[n], key + ' ' + n);
  assert.deepStrictEqual(lf.resolvedOptions(), { locale: 'en', type, style });
}

const lf = new Intl.ListFormat('en', { style: 'long', type: 'disjunction' });
assert.deepStrictEqual(lf.formatToParts(['x', 'y', 'z']), [
  { type: 'element', value: 'x' }, { type: 'literal', value: ', ' },
  { type: 'element', value: 'y' }, { type: 'literal', value: ', or ' },
  { type: 'element', value: 'z' },
]);
assert.deepStrictEqual(lf.formatToParts([]), []);
assert.strictEqual(lf.format(), '');
assert.strictEqual(lf.format(new Set(['p', 'q'])), 'p or q');
assert.deepStrictEqual(new Intl.ListFormat().resolvedOptions().type, 'conjunction');
assert.strictEqual(new Intl.ListFormat().resolvedOptions().style, 'long');
assert.strictEqual(new Intl.ListFormat(undefined, undefined).format(['a', 'b']), 'a and b');

assert.throws(() => Intl.ListFormat('en'), TypeError);
assert.throws(() => new Intl.ListFormat('en', 'x'), TypeError);
assert.throws(() => new Intl.ListFormat('en', { type: 'bad' }), RangeError);
assert.throws(() => new Intl.ListFormat('en', { style: 'bad' }), RangeError);
assert.throws(() => new Intl.ListFormat('en', { localeMatcher: 'bad' }), RangeError);
assert.throws(() => new Intl.ListFormat('en_US'), RangeError);
assert.throws(() => lf.format([1]), TypeError);
assert.throws(() => lf.format(['a', 1]), TypeError);
assert.throws(() => lf.formatToParts([null]), TypeError);

// receivers must be real ListFormat instances; look-alike objects and other
// Intl instances are rejected rather than read
const { format, formatToParts, resolvedOptions } = Intl.ListFormat.prototype;
for (const bad of [{}, { listType: 99, listStyle: 99, locale: 'en' }, new Intl.NumberFormat('en'), Object.create(Intl.ListFormat.prototype)]) {
  assert.throws(() => format.call(bad, ['a', 'b']), TypeError);
  assert.throws(() => formatToParts.call(bad, ['a', 'b']), TypeError);
  assert.throws(() => resolvedOptions.call(bad), TypeError);
}
assert.throws(() => format.call(undefined, ['a']), TypeError);
assert.strictEqual(new Intl.ListFormat('en').format(['← x', '—', 'café']), '← x, —, and café');

assert.strictEqual(Intl.ListFormat.length, 0);
assert.strictEqual(Intl.ListFormat.prototype.format.length, 1);
assert.strictEqual(Intl.ListFormat.prototype.formatToParts.length, 1);
assert.strictEqual(Intl.ListFormat.prototype.resolvedOptions.length, 0);
assert.strictEqual(Object.prototype.toString.call(lf), '[object Intl.ListFormat]');
assert.deepStrictEqual(Intl.ListFormat.supportedLocalesOf(['en-US']), ['en-US']);
// IteratorClose runs when an element is not a string
{
  let closed = 0;
  const iterable = { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; }, return() { closed++; return {}; } }; } };
  assert.throws(() => lf.format(iterable), TypeError);
  assert.strictEqual(closed, 1);
}

// Symbol option values are a TypeError, not a RangeError
assert.throws(() => new Intl.ListFormat('en', { type: Symbol() }), TypeError);
assert.throws(() => new Intl.ListFormat('en', { style: Symbol() }), TypeError);

// locale is internal state, not an own property
{
  class Sub extends Intl.ListFormat { locale = 'x'; }
  assert.strictEqual(new Sub('en').resolvedOptions().locale, 'en');
  const own = new Intl.ListFormat('en');
  assert.strictEqual(Object.getOwnPropertyNames(own).includes('locale'), false);
  delete own.locale;
  Object.defineProperty(own, 'locale', { value: 'zz' });
  assert.strictEqual(own.resolvedOptions().locale, 'en');
}

// a bad element must not stringify the whole list into the message
{
  const huge = new Array(200000).fill('a');
  huge.push(1);
  huge.push(...new Array(1000).fill('b'));
  try { lf.format(huge); assert.fail('expected throw'); } catch (e) {
    assert.ok(e instanceof TypeError);
    assert.ok(e.message.length < 200, e.message.length);
  }
}

console.log('PASS Intl.ListFormat');
