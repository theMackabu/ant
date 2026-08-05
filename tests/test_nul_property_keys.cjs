// Property keys containing an embedded NUL must behave like any other key. The intern
// table always stored the full bytes; the bugs were consumers recomputing lengths with
// strlen (fixed via intern_length) and the dynamic-get path flattening keys to C strings
// (fixed via js_getprop_fallback_len). Every expectation verified against node first.
function assert(c, m) { if (!c) { console.log('FAIL:', m); process.exit(1); } }

const k = 'a' + String.fromCharCode(0) + 'b';
const o = {};
o[k] = 1;
o.other = 2;

assert(k.length === 3, 'key length');
assert(Object.keys(o).length === 2, 'two keys');
assert(Object.keys(o)[0] === k, 'full key enumerated');
assert(Object.keys(o)[0].length === 3, 'enumerated key length');
assert(Object.hasOwn(o, k), 'hasOwn');
assert(o[k] === 1, 'dynamic get');
assert(o['a'] === undefined, 'prefix is a different key');
assert(JSON.stringify(o) === '{"a\\u0000b":1,"other":2}', 'stringify escapes the NUL');
assert(JSON.parse(JSON.stringify(o))[k] === 1, 'round trip');
delete o[k];
assert(!Object.hasOwn(o, k), 'delete');
assert(o.other === 2, 'sibling untouched');

const nested = { [k]: { [k]: 'deep' } };
assert(nested[k][k] === 'deep', 'nested NUL keys');

console.log('PASS');
