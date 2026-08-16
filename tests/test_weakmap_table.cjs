function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const count = 20000;
const keys = new Array(count);
const map = new WeakMap();

for (let i = 0; i < count; i++) {
  keys[i] = { i };
  map.set(keys[i], i);
}

for (let i = 0; i < count; i++)
  assert(map.get(keys[i]) === i, `initial lookup failed at ${i}`);

for (let i = 0; i < count; i += 2)
  assert(map.delete(keys[i]), `delete failed at ${i}`);

for (let i = 0; i < count; i++) {
  if ((i & 1) === 0) {
    assert(!map.has(keys[i]), `deleted key remained at ${i}`);
  } else {
    map.set(keys[i], i + 1);
    assert(map.get(keys[i]) === i + 1, `update failed at ${i}`);
  }
}

const replacements = new Array(count / 2);
for (let i = 0; i < replacements.length; i++) {
  const key = { replacement: i };
  replacements[i] = key;
  map.set(key, -i);
}

for (let i = 0; i < replacements.length; i++)
  assert(map.get(replacements[i]) === -i, `replacement failed at ${i}`);

const fn = () => {};
const symbol = Symbol("weak-table");
map.set(fn, "function");
map.set(symbol, "symbol");
assert(map.get(fn) === "function", "function identity lookup failed");
assert(map.get(symbol) === "symbol", "symbol identity lookup failed");

console.log("weakmap-table:ok");
