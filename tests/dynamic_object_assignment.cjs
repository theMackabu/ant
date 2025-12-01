// Test dynamic object property assignment

console.log("=== Dynamic Object Assignment Tests ===\n");

// Test 1: Basic dynamic property assignment using bracket notation
console.log("Test 1: Basic bracket notation assignment");
let obj1 = {};
obj1["name"] = "Alice";
obj1["age"] = 30;
obj1["active"] = true;
console.log("  obj1.name:", obj1["name"]);
console.log("  obj1.age:", obj1["age"]);
console.log("  obj1.active:", obj1["active"]);

// Test 2: Dynamic property assignment with variable keys
console.log("\nTest 2: Variable keys");
let obj2 = {};
let key1 = "firstName";
let key2 = "lastName";
let key3 = "email";
obj2[key1] = "Bob";
obj2[key2] = "Smith";
obj2[key3] = "bob@example.com";
console.log("  obj2[key1]:", obj2[key1]);
console.log("  obj2[key2]:", obj2[key2]);
console.log("  obj2[key3]:", obj2[key3]);

// Test 3: Numeric keys (array-like object)
console.log("\nTest 3: Numeric keys");
let obj3 = {};
obj3[0] = "first";
obj3[1] = "second";
obj3[2] = "third";
console.log("  obj3[0]:", obj3[0]);
console.log("  obj3[1]:", obj3[1]);
console.log("  obj3[2]:", obj3[2]);

// Test 4: Dynamic property assignment in loop
console.log("\nTest 4: Loop assignment");
let obj4 = {};
for (let i = 0; i < 5; i++) {
  obj4[i] = i * 10;
}
console.log("  obj4[0]:", obj4[0]);
console.log("  obj4[2]:", obj4[2]);
console.log("  obj4[4]:", obj4[4]);

// Test 5: Mix of dot notation and bracket notation
console.log("\nTest 5: Mixed notation");
let obj5 = {};
obj5.static = "dot notation";
obj5["dynamic"] = "bracket notation";
let propName = "computed";
obj5[propName] = "variable key";
console.log("  obj5.static:", obj5.static);
console.log("  obj5['dynamic']:", obj5["dynamic"]);
console.log("  obj5[propName]:", obj5[propName]);

// Test 6: Overwriting existing properties
console.log("\nTest 6: Overwrite properties");
let obj6 = { name: "Original" };
console.log("  Before:", obj6.name);
obj6["name"] = "Updated";
console.log("  After:", obj6["name"]);

// Test 7: Dynamic keys with string concatenation
console.log("\nTest 7: Concatenated keys");
let obj7 = {};
let prefix = "prop";
obj7[prefix + "1"] = "value1";
obj7[prefix + "2"] = "value2";
obj7[prefix + "3"] = "value3";
console.log("  obj7['prop1']:", obj7["prop1"]);
console.log("  obj7['prop2']:", obj7["prop2"]);
console.log("  obj7['prop3']:", obj7["prop3"]);

// Test 8: Building object dynamically from arrays
console.log("\nTest 8: Build from arrays");
let keys = ["x", "y", "z"];
let values = [100, 200, 300];
let obj8 = {};
for (let i = 0; i < keys.length; i++) {
  obj8[keys[i]] = values[i];
}
console.log("  obj8.x:", obj8["x"]);
console.log("  obj8.y:", obj8["y"]);
console.log("  obj8.z:", obj8["z"]);

// Test 9: Nested dynamic assignment
console.log("\nTest 9: Nested objects");
let obj9 = {};
obj9["user"] = {};
obj9["user"]["name"] = "Charlie";
obj9["user"]["age"] = 25;
console.log("  obj9.user.name:", obj9["user"]["name"]);
console.log("  obj9.user.age:", obj9["user"]["age"]);

// Test 10: Reading undefined dynamic properties
console.log("\nTest 10: Undefined properties");
let obj10 = { a: 1 };
console.log("  obj10['a']:", obj10["a"]);
console.log("  obj10['b'] (undefined):", obj10["b"]);

// Test 10a: Dynamic access with literal strings
console.log("\nTest 10a: Literal string keys");
let cat = { meow: "purr", sound: "meow meow", age: 3 };
console.log("  cat['meow']:", cat["meow"]);
console.log("  cat['sound']:", cat["sound"]);
console.log("  cat['age']:", cat["age"]);
let key = "meow";
console.log("  cat[key] where key='meow':", cat[key]);

// Test 11: Dynamic property assignment with expressions
console.log("\nTest 11: Expression keys");
let obj11 = {};
obj11[1 + 1] = "two";
obj11[2 + 2] = "four";
console.log("  obj11[2]:", obj11[2]);
console.log("  obj11[4]:", obj11[4]);

// Test 12: Creating a map-like structure
console.log("\nTest 12: Map-like usage");
let map = {};
map["key1"] = { value: 10, type: "number" };
map["key2"] = { value: "hello", type: "string" };
map["key3"] = { value: true, type: "boolean" };
console.log("  map['key1'].value:", map["key1"]["value"]);
console.log("  map['key2'].value:", map["key2"]["value"]);
console.log("  map['key3'].value:", map["key3"]["value"]);

console.log("\n=== All tests completed ===");
