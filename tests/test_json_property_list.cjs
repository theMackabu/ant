// JSON.stringify's replacer array is a PropertyList: listed keys are read
// with [[Get]] (inherited and non-enumerable too), in list order, without
// duplicates. Keys with NUL characters keep their full length everywhere.
// Found by tests/differential (json family). Expected values match V8.
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

same(JSON.stringify(Object.create({ a: 1 }), ['a']), '{"a":1}', 'inherited key');
same(JSON.stringify({ b: 1, a: 2, c: 3 }, ['a', 'b', 'a', 'zz']), '{"a":2,"b":1}', 'list order, duplicates, missing keys');
same(JSON.stringify({ '1.5': 1, 1: 2, '1e+21': 3 }, [1.5, 1, 1e21]), '{"1.5":1,"1":2,"1e+21":3}', 'numbers use ToString');
same(JSON.stringify({ x: 1, 1: 2 }, [new String('x'), new Number(1), {}, null]), '{"x":1,"1":2}', 'wrapper objects');
same(JSON.stringify(Object.defineProperty({}, 'hidden', { value: 1 }), ['hidden']), '{"hidden":1}', 'non-enumerable key');
same(JSON.stringify([1, { x: 1, y: 2 }], ['y']), '[1,{"y":2}]', 'arrays keep every element');
same(JSON.stringify({ a: 1, b: 2 }, ['b', 'a'], 1), '{\n "b": 2,\n "a": 1\n}', 'with indent');

same(JSON.stringify({ 'a\0b': 5, 1: 1 }), '{"1":1,"a\\u0000b":5}', 'NUL key next to an index key');
let threw = false;
try { JSON.stringify({ 'a\0b': 10n, 1: 1 }); } catch (e) { threw = e instanceof TypeError; }
same(threw, true, 'BigInt under a NUL key still throws');
same(JSON.stringify({ 'a\0b': 1 }, (k, v) => (typeof v === 'number' ? k.length : v)), '{"a\\u0000b":3}', 'replacer sees the full key');
same(JSON.stringify({ 'k\0': { toJSON(k) { return k.length; } } }), '{"k\\u0000":2}', 'toJSON sees the full key');
same(JSON.stringify(JSON.parse('{"a\\u0000b":1,"c":2}', (k, v) => (typeof v === 'number' ? k.length : v))), '{"a\\u0000b":3,"c":1}', 'reviver sees the full key');

console.log('json property list ok');
