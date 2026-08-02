// Object.defineProperty on a dense array index: a store-equivalent descriptor (value +
// writable/enumerable/configurable all explicitly true, no accessor) takes a dense fast
// path; everything else materializes the dense elements into real properties. Every
// expectation here was verified against node first.
function assert(c, m) { if (!c) { console.log('FAIL:', m); process.exit(1); } }
const D = { writable: true, enumerable: true, configurable: true };

// store-equivalent: value lands, array stays enumerable-ordered, descriptor reads back all-true
{
  const a = [1, 2, 3];
  Object.defineProperty(a, '1', { value: 9, ...D });
  assert(a[1] === 9, 'fast path stores the value');
  const d = Object.getOwnPropertyDescriptor(a, '1');
  assert(d.value === 9 && d.writable && d.enumerable && d.configurable, 'descriptor reads back');
  assert(Object.keys(a).join(',') === '0,1,2', 'key order');
  assert(JSON.stringify(a) === '[1,9,3]', 'json');
}
// beyond length extends like a plain store
{
  const a = [1];
  Object.defineProperty(a, '5', { value: 7, ...D });
  assert(a.length === 6 && a[5] === 7, 'length extension');
}
// partial descriptor on an EXISTING element keeps its attributes (all true for elements)
{
  const a = [1, 2, 3];
  Object.defineProperty(a, '1', { value: 9 });
  const d = Object.getOwnPropertyDescriptor(a, '1');
  assert(d.writable && d.enumerable && d.configurable, 'existing element keeps attributes');
  assert(a[1] === 9, 'value updated');
}
// partial descriptor on a NEW index defaults every attribute to false
{
  const a = [1, 2, 3];
  Object.defineProperty(a, '5', { value: 9 });
  const d = Object.getOwnPropertyDescriptor(a, '5');
  assert(d.writable === false && d.enumerable === false && d.configurable === false, 'new prop defaults false');
  assert(a.length === 6, 'length extended');
  let threw = false;
  try { Object.defineProperty(a, '5', { value: 10, writable: true }); } catch (e) { threw = true; }
  assert(threw, 'non-configurable redefine throws');
}
// accessors and the dense element they shadow
{
  const a = [1, 2, 3];
  Object.defineProperty(a, '1', { get() { return 42; }, configurable: true, enumerable: true });
  assert(a[1] === 42, 'getter shadows dense element');
  assert(JSON.stringify(a) === '[1,42,3]', 'json consults getter');
}
// non-canonical keys are plain string keys, never elements
{
  const a = [1, 2, 3];
  for (const k of ['01', '+1', '1e0', '4294967295']) Object.defineProperty(a, k, { value: 'K' + k, ...D });
  assert(a.length === 3 && a[1] === 2, 'array untouched by non-canonical keys');
  assert(Object.getOwnPropertyDescriptor(a, '01').value === 'K01', 'string key stored');
}
// preventExtensions: existing index redefine OK, new index throws
{
  const a = [1, 2, 3];
  Object.preventExtensions(a);
  Object.defineProperty(a, '1', { value: 9, ...D });
  assert(a[1] === 9, 'existing index redefine on non-extensible');
  let threw = false;
  try { Object.defineProperty(a, '5', { value: 9, ...D }); } catch (e) { threw = true; }
  assert(threw, 'new index on non-extensible throws');
}
// sealed: configurable:true must throw, value unchanged
{
  const a = Object.seal([1, 2, 3]);
  let threw = false;
  try { Object.defineProperty(a, '1', { value: 9, ...D }); } catch (e) { threw = true; }
  assert(threw && a[1] === 2, 'sealed redefine with configurable:true throws');
}
// frozen: define fails, value unchanged
{
  const a = Object.freeze([1, 2, 3]);
  let threw = false;
  try { Object.defineProperty(a, '1', { value: 9, ...D }); } catch (e) { threw = true; }
  assert(threw && a[1] === 2, 'frozen define throws');
}
// hole filling
{
  const a = [1, , 3];
  Object.defineProperty(a, '1', { value: 2, ...D });
  assert(a[1] === 2 && (1 in a), 'hole filled');
}
console.log('PASS');
