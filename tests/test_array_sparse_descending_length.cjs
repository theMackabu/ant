const assert = require("node:assert");

const arr = [];
for (let n = 128, i = 0; i < 111; i++) {
  arr[--n] = i;
}

assert.equal(arr.length, 128);
assert.equal(arr[127], 0);
assert.equal(arr[31], 96);
assert.equal(arr[30], 97);
assert.equal(arr[17], 110);
assert.equal(arr[16], undefined);
assert.equal(Object.keys(arr).length, 111);

const mixed = [];
mixed[127] = "tail";
mixed[31] = "lower";

assert.equal(mixed.length, 128);
assert.equal(mixed[127], "tail");
assert.equal(mixed[31], "lower");

console.log("OK: sparse descending array writes preserve length");
