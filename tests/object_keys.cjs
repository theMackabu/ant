// Test Object.keys() functionality

console.log("=== Object.keys() Tests ===\n");

// Test 1: Basic object with string keys
console.log("Test 1: Basic object");
let obj1 = { a: 1, b: 2, c: 3 };
let keys1 = Object.keys(obj1);
console.log("  Keys length:", keys1.length);
for (let i = 0; i < keys1.length; i++) {
  console.log("    " + keys1[i] + ": " + obj1[keys1[i]]);
}

// Test 2: Empty object
console.log("\nTest 2: Empty object");
let obj2 = {};
let keys2 = Object.keys(obj2);
console.log("  Keys length:", keys2.length);

// Test 3: Object with various property types
console.log("\nTest 3: Object with various properties");
let obj3 = {
  name: "John",
  age: 30,
  active: true
};
let keys3 = Object.keys(obj3);
console.log("  Keys found:");
for (let i = 0; i < keys3.length; i++) {
  console.log("    " + keys3[i]);
}

// Test 4: Iterating using Object.keys
console.log("\nTest 4: Iterate using Object.keys");
let person = {
  firstName: "Alice",
  lastName: "Smith",
  age: 25
};
let personKeys = Object.keys(person);
console.log("  Person properties:");
for (let i = 0; i < personKeys.length; i++) {
  let key = personKeys[i];
  console.log("    " + key + " = " + person[key]);
}

// Test 5: Object.keys with array
console.log("\nTest 5: Object.keys with array");
let arr = ["a", "b", "c"];
let arrKeys = Object.keys(arr);
console.log("  Array keys length:", arrKeys.length);
console.log("  Array keys:");
for (let i = 0; i < arrKeys.length; i++) {
  console.log("    " + arrKeys[i]);
}

// Test 6: Nested object iteration
console.log("\nTest 6: Nested object");
let nested = {
  user: {
    name: "Bob",
    email: "bob@example.com"
  },
  settings: {
    theme: "dark",
    notifications: true
  }
};
let nestedKeys = Object.keys(nested);
console.log("  Top-level keys:");
for (let i = 0; i < nestedKeys.length; i++) {
  console.log("    " + nestedKeys[i]);
}

// Test 7: Using Object.keys for validation
console.log("\nTest 7: Key validation");
let config = {
  host: "localhost",
  port: 8080,
  debug: true
};
let requiredKeys = ["host", "port"];
let configKeys = Object.keys(config);
console.log("  Config has " + configKeys.length + " keys");
console.log("  Required keys present:");
for (let i = 0; i < requiredKeys.length; i++) {
  let hasKey = false;
  for (let j = 0; j < configKeys.length; j++) {
    if (requiredKeys[i] === configKeys[j]) {
      hasKey = true;
    }
  }
  console.log("    " + requiredKeys[i] + ": " + (hasKey ? "yes" : "no"));
}

// Test 8: Copy object using Object.keys
console.log("\nTest 8: Copy object");
let original = { x: 10, y: 20, z: 30 };
let copy = {};
let originalKeys = Object.keys(original);
for (let i = 0; i < originalKeys.length; i++) {
  let key = originalKeys[i];
  copy[key] = original[key];
}
let copyKeys = Object.keys(copy);
console.log("  Original keys:", originalKeys.length);
console.log("  Copy keys:", copyKeys.length);
console.log("  Copy values:");
for (let i = 0; i < copyKeys.length; i++) {
  console.log("    " + copyKeys[i] + " = " + copy[copyKeys[i]]);
}

console.log("\n=== All tests completed ===");
