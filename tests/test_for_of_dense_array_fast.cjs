function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dense = [1, undefined, 3];
const denseValues = [];
for (const value of dense) denseValues.push(value);
assert(denseValues.length === 3, "dense iteration length");
assert(denseValues[0] === 1 && denseValues[1] === undefined && denseValues[2] === 3,
  "dense iteration values");

Array.prototype[1] = 42;
try {
  const holey = [1, , 3];
  const holeyValues = [];
  for (const value of holey) holeyValues.push(value);
  assert(holeyValues.length === 3, "holey iteration length");
  assert(holeyValues[1] === 42, "array holes must consult the prototype");
} finally {
  delete Array.prototype[1];
}

const growing = [1];
const growingValues = [];
for (const value of growing) {
  growingValues.push(value);
  if (value === 1) growing.push(2);
}
assert(growingValues.length === 2 && growingValues[1] === 2,
  "array iteration must observe length growth");

const shrinking = [1, 2, 3];
const shrinkingValues = [];
for (const value of shrinking) {
  shrinkingValues.push(value);
  shrinking.length = 1;
}
assert(shrinkingValues.length === 1, "array iteration must observe length shrinkage");

console.log("PASS");
