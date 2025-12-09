// Test Object.prototype.toString directly

console.log("Testing toString:");

// Test on objects directly
const obj = {};
console.log("obj.toString():", obj.toString());

const arr = [];
console.log("arr.toString():", arr.toString());

const atomics = Atomics;
console.log("Atomics object:", atomics);
console.log("Atomics.toString:", atomics.toString);

// If toString exists, call it
if (atomics.toString) {
  console.log("Calling Atomics.toString():", atomics.toString());
}

// Test custom object with @@toStringTag
const custom = { "@@toStringTag": "MyCustomType" };
console.log("custom.toString():", custom.toString());
