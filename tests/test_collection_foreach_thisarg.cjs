function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const mapOut = [];
new Map([
  ["a", 1],
  ["b", 2],
]).forEach(function (value, key, map) {
  assert(map instanceof Map, "Map.forEach should pass the map as the third argument");
  this.push(key + ":" + value);
}, mapOut);
assert(JSON.stringify(mapOut) === '["a:1","b:2"]', "Map.forEach should bind thisArg");

const setOut = [];
new Set([1, 2]).forEach(function (value, key, set) {
  assert(value === key, "Set.forEach should pass value twice");
  assert(set instanceof Set, "Set.forEach should pass the set as the third argument");
  this.push(value);
}, setOut);
assert(JSON.stringify(setOut) === "[1,2]", "Set.forEach should bind thisArg");

const arrayOut = [];
[1, 2].forEach(function (value) {
  this.push(value);
}, arrayOut);
assert(JSON.stringify(arrayOut) === "[1,2]", "Array.forEach should continue to bind thisArg");

console.log("collection:foreach-thisarg:ok");
