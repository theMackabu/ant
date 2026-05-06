function assert(condition, message) {
  if (!condition) {
    console.log("FAIL: " + message);
    throw new Error(message);
  }
}

Array.prototype[0] = 9;
Array.prototype[1] = [7, 8];

try {
  const flatResult = [, 1].flat();
  assert(JSON.stringify(flatResult) === "[9,1]", "flat should read inherited index values");

  const flatMapResult = [, 2].flatMap(function (value) {
    return [value];
  });
  assert(JSON.stringify(flatMapResult) === "[9,2]", "flatMap should visit inherited index values");

  const nestedResult = [, 3].flatMap(function (_value, index) {
    return index === 0 ? Array.prototype[1] : [_value];
  });
  assert(JSON.stringify(nestedResult) === "[7,8,3]", "flatMap should flatten inherited array results");
} finally {
  delete Array.prototype[0];
  delete Array.prototype[1];
}

console.log("OK");
