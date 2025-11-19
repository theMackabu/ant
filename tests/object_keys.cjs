// Test Object.keys() functionality

Ant.println("=== Object.keys() Tests ===\n");

// Test 1: Basic object with string keys
Ant.println("Test 1: Basic object");
let obj1 = { a: 1, b: 2, c: 3 };
let keys1 = Object.keys(obj1);
Ant.println("  Keys length:", keys1.length);
for (let i = 0; i < keys1.length; i++) {
  Ant.println("    " + keys1[i] + ": " + obj1[keys1[i]]);
}

// Test 2: Empty object
Ant.println("\nTest 2: Empty object");
let obj2 = {};
let keys2 = Object.keys(obj2);
Ant.println("  Keys length:", keys2.length);

// Test 3: Object with various property types
Ant.println("\nTest 3: Object with various properties");
let obj3 = {
  name: "John",
  age: 30,
  active: true
};
let keys3 = Object.keys(obj3);
Ant.println("  Keys found:");
for (let i = 0; i < keys3.length; i++) {
  Ant.println("    " + keys3[i]);
}

// Test 4: Iterating using Object.keys
Ant.println("\nTest 4: Iterate using Object.keys");
let person = {
  firstName: "Alice",
  lastName: "Smith",
  age: 25
};
let personKeys = Object.keys(person);
Ant.println("  Person properties:");
for (let i = 0; i < personKeys.length; i++) {
  let key = personKeys[i];
  Ant.println("    " + key + " = " + person[key]);
}

// Test 5: Object.keys with array
Ant.println("\nTest 5: Object.keys with array");
let arr = ["a", "b", "c"];
let arrKeys = Object.keys(arr);
Ant.println("  Array keys length:", arrKeys.length);
Ant.println("  Array keys:");
for (let i = 0; i < arrKeys.length; i++) {
  Ant.println("    " + arrKeys[i]);
}

// Test 6: Nested object iteration
Ant.println("\nTest 6: Nested object");
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
Ant.println("  Top-level keys:");
for (let i = 0; i < nestedKeys.length; i++) {
  Ant.println("    " + nestedKeys[i]);
}

// Test 7: Using Object.keys for validation
Ant.println("\nTest 7: Key validation");
let config = {
  host: "localhost",
  port: 8080,
  debug: true
};
let requiredKeys = ["host", "port"];
let configKeys = Object.keys(config);
Ant.println("  Config has " + configKeys.length + " keys");
Ant.println("  Required keys present:");
for (let i = 0; i < requiredKeys.length; i++) {
  let hasKey = false;
  for (let j = 0; j < configKeys.length; j++) {
    if (requiredKeys[i] === configKeys[j]) {
      hasKey = true;
    }
  }
  Ant.println("    " + requiredKeys[i] + ": " + (hasKey ? "yes" : "no"));
}

// Test 8: Copy object using Object.keys
Ant.println("\nTest 8: Copy object");
let original = { x: 10, y: 20, z: 30 };
let copy = {};
let originalKeys = Object.keys(original);
for (let i = 0; i < originalKeys.length; i++) {
  let key = originalKeys[i];
  copy[key] = original[key];
}
let copyKeys = Object.keys(copy);
Ant.println("  Original keys:", originalKeys.length);
Ant.println("  Copy keys:", copyKeys.length);
Ant.println("  Copy values:");
for (let i = 0; i < copyKeys.length; i++) {
  Ant.println("    " + copyKeys[i] + " = " + copy[copyKeys[i]]);
}

Ant.println("\n=== All tests completed ===");
