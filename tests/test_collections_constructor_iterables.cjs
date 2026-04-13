function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const map = new Map([
  ['x', 1],
  ['y', 2],
]);
const mapCopy = new Map(map);
const mapNull = new Map(null);
const mapUndef = new Map(undefined);

assert(mapCopy.size === 2, `expected copied map size 2, got ${mapCopy.size}`);
assert(mapCopy.get('x') === 1, 'expected copied map to contain x => 1');
assert(mapCopy.get('y') === 2, 'expected copied map to contain y => 2');
assert(mapNull.size === 0, `expected null map size 0, got ${mapNull.size}`);
assert(mapUndef.size === 0, `expected undefined map size 0, got ${mapUndef.size}`);

const set = new Set(['a', 'b']);
const setCopy = new Set(set);
const setNull = new Set(null);
const setUndef = new Set(undefined);

assert(setCopy.size === 2, `expected copied set size 2, got ${setCopy.size}`);
assert(setCopy.has('a'), 'expected copied set to contain a');
assert(setCopy.has('b'), 'expected copied set to contain b');
assert(setNull.size === 0, `expected null set size 0, got ${setNull.size}`);
assert(setUndef.size === 0, `expected undefined set size 0, got ${setUndef.size}`);

console.log('collections constructor iterables ok');
