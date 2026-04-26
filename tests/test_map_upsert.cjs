function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const map = new Map([["a", 1]]);
let updateCalls = 0;
let insertCalls = 0;

const updated = map.upsert(
  "a",
  (value, key, receiver) => {
    updateCalls++;
    assert(value === 1, "Map update callback receives existing value");
    assert(key === "a", "Map update callback receives key");
    assert(receiver === map, "Map update callback receives receiver");
    return 2;
  },
  () => {
    insertCalls++;
    return 3;
  }
);

assert(updated === 2, "Map.upsert returns updated value");
assert(map.get("a") === 2, "Map.upsert stores updated value");
assert(updateCalls === 1, "Map update callback called once");
assert(insertCalls === 0, "Map insert callback not called for existing key");

const inserted = map.upsert(
  "b",
  () => {
    updateCalls++;
    return 4;
  },
  (key, receiver) => {
    insertCalls++;
    assert(key === "b", "Map insert callback receives key");
    assert(receiver === map, "Map insert callback receives receiver");
    return 3;
  }
);

assert(inserted === 3, "Map.upsert returns inserted value");
assert(Array.from(map).join() === "a,2,b,3", "Map.upsert inserts missing key");
assert(updateCalls === 1, "Map update callback not called for missing key");
assert(insertCalls === 1, "Map insert callback called once");

const a = {};
const b = {};
const weak = new WeakMap([[a, 1]]);

assert(weak.upsert(a, value => value + 1, () => 3) === 2, "WeakMap.upsert returns updated value");
assert(weak.get(a) === 2, "WeakMap.upsert stores updated value");
assert(weak.upsert(b, () => 4, () => 3) === 3, "WeakMap.upsert returns inserted value");
assert(weak.get(b) === 3, "WeakMap.upsert stores inserted value");

console.log("Map/WeakMap upsert tests completed!");
