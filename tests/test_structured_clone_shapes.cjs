// structuredClone keeps primitive wrappers, full-length keys, array holes and
// named array properties. Found by tests/differential (structured-clone
// family). Expected values match V8.
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}
const tag = v => Object.prototype.toString.call(v);

for (const [value, expected] of [[new Number(-0), '[object Number]'], [new String('s'), '[object String]'], [new Boolean(false), '[object Boolean]'], [Object(1n), '[object BigInt]']]) {
  const copy = structuredClone(value);
  same(tag(copy), expected, `${expected} wrapper`);
  same(Object.is(copy.valueOf(), value.valueOf()), true, `${expected} primitive`);
}
let error = null;
try { structuredClone(Object(Symbol('s'))); } catch (e) { error = e; }
same(error && error.name, 'DataCloneError', 'Symbol wrapper');

same(JSON.stringify(Object.keys(structuredClone({ 'a\0b': 1, c: 2 }))), '["a\\u0000b","c"]', 'NUL key');

const sparse = [, 1, , 2];
sparse.extra = 'x';
const copy = structuredClone(sparse);
same(JSON.stringify(Object.keys(copy)), '["1","3","extra"]', 'holes and named properties');
same(copy.length, 4, 'length');
same(structuredClone(new Array(3)).length, 3, 'trailing holes');
same(JSON.stringify(structuredClone([1, [2, 3], { a: [4] }])), '[1,[2,3],{"a":[4]}]', 'dense arrays');

console.log('structured clone shapes ok');
