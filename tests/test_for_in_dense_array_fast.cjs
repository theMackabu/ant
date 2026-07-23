function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function collectKeys(object) {
  const keys = [];
  for (const key in object) keys.push(key);
  return keys.join(",");
}

for (let i = 0; i < 500; i++) {
  assertEq(collectKeys([1, 2, 3]), "0,1,2", "warm dense array");
}

assertEq(collectKeys([1, 2, 3]), "0,1,2", "hot dense array");
assertEq(collectKeys([1, , 3]), "0,2", "dense array hole");

const ownNamed = [1];
ownNamed.named = true;
assertEq(collectKeys(ownNamed), "0,named", "enumerable own named property");

const sparse = [];
sparse[1000] = true;
assertEq(collectKeys(sparse), "1000", "sparse array index");

const inheritedProto = Object.create(Array.prototype);
inheritedProto.inherited = true;
const inherited = [1, 2];
Object.setPrototypeOf(inherited, inheritedProto);
assertEq(collectKeys(inherited), "0,1,inherited", "enumerable prototype property");

const duplicateProto = Object.create(Array.prototype);
duplicateProto[0] = "prototype";
const duplicate = ["own"];
Object.setPrototypeOf(duplicate, duplicateProto);
assertEq(collectKeys(duplicate), "0", "own key suppresses inherited duplicate");

const hiddenProto = Object.create(Array.prototype);
Object.defineProperty(hiddenProto, "hidden", {
  value: true,
  enumerable: false,
});
const hidden = [1];
Object.setPrototypeOf(hidden, hiddenProto);
assertEq(collectKeys(hidden), "0", "non-enumerable prototype property");

let ownKeysCalls = 0;
const proxy = new Proxy([1, 2], {
  ownKeys(target) {
    ownKeysCalls++;
    return Reflect.ownKeys(target);
  },
});
collectKeys(proxy);
assertEq(ownKeysCalls, 1, "proxy ownKeys trap");

console.log("OK: test_for_in_dense_array_fast");
