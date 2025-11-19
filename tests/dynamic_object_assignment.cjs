// Test dynamic object property assignment

Ant.println("=== Dynamic Object Assignment Tests ===\n");

// Test 1: Basic dynamic property assignment using bracket notation
Ant.println("Test 1: Basic bracket notation assignment");
let obj1 = {};
obj1["name"] = "Alice";
obj1["age"] = 30;
obj1["active"] = true;
Ant.println("  obj1.name:", obj1["name"]);
Ant.println("  obj1.age:", obj1["age"]);
Ant.println("  obj1.active:", obj1["active"]);

// Test 2: Dynamic property assignment with variable keys
Ant.println("\nTest 2: Variable keys");
let obj2 = {};
let key1 = "firstName";
let key2 = "lastName";
let key3 = "email";
obj2[key1] = "Bob";
obj2[key2] = "Smith";
obj2[key3] = "bob@example.com";
Ant.println("  obj2[key1]:", obj2[key1]);
Ant.println("  obj2[key2]:", obj2[key2]);
Ant.println("  obj2[key3]:", obj2[key3]);

// Test 3: Numeric keys (array-like object)
Ant.println("\nTest 3: Numeric keys");
let obj3 = {};
obj3[0] = "first";
obj3[1] = "second";
obj3[2] = "third";
Ant.println("  obj3[0]:", obj3[0]);
Ant.println("  obj3[1]:", obj3[1]);
Ant.println("  obj3[2]:", obj3[2]);

// Test 4: Dynamic property assignment in loop
Ant.println("\nTest 4: Loop assignment");
let obj4 = {};
for (let i = 0; i < 5; i++) {
  obj4[i] = i * 10;
}
Ant.println("  obj4[0]:", obj4[0]);
Ant.println("  obj4[2]:", obj4[2]);
Ant.println("  obj4[4]:", obj4[4]);

// Test 5: Mix of dot notation and bracket notation
Ant.println("\nTest 5: Mixed notation");
let obj5 = {};
obj5.static = "dot notation";
obj5["dynamic"] = "bracket notation";
let propName = "computed";
obj5[propName] = "variable key";
Ant.println("  obj5.static:", obj5.static);
Ant.println("  obj5['dynamic']:", obj5["dynamic"]);
Ant.println("  obj5[propName]:", obj5[propName]);

// Test 6: Overwriting existing properties
Ant.println("\nTest 6: Overwrite properties");
let obj6 = { name: "Original" };
Ant.println("  Before:", obj6.name);
obj6["name"] = "Updated";
Ant.println("  After:", obj6["name"]);

// Test 7: Dynamic keys with string concatenation
Ant.println("\nTest 7: Concatenated keys");
let obj7 = {};
let prefix = "prop";
obj7[prefix + "1"] = "value1";
obj7[prefix + "2"] = "value2";
obj7[prefix + "3"] = "value3";
Ant.println("  obj7['prop1']:", obj7["prop1"]);
Ant.println("  obj7['prop2']:", obj7["prop2"]);
Ant.println("  obj7['prop3']:", obj7["prop3"]);

// Test 8: Building object dynamically from arrays
Ant.println("\nTest 8: Build from arrays");
let keys = ["x", "y", "z"];
let values = [100, 200, 300];
let obj8 = {};
for (let i = 0; i < keys.length; i++) {
  obj8[keys[i]] = values[i];
}
Ant.println("  obj8.x:", obj8["x"]);
Ant.println("  obj8.y:", obj8["y"]);
Ant.println("  obj8.z:", obj8["z"]);

// Test 9: Nested dynamic assignment
Ant.println("\nTest 9: Nested objects");
let obj9 = {};
obj9["user"] = {};
obj9["user"]["name"] = "Charlie";
obj9["user"]["age"] = 25;
Ant.println("  obj9.user.name:", obj9["user"]["name"]);
Ant.println("  obj9.user.age:", obj9["user"]["age"]);

// Test 10: Reading undefined dynamic properties
Ant.println("\nTest 10: Undefined properties");
let obj10 = { a: 1 };
Ant.println("  obj10['a']:", obj10["a"]);
Ant.println("  obj10['b'] (undefined):", obj10["b"]);

// Test 10a: Dynamic access with literal strings
Ant.println("\nTest 10a: Literal string keys");
let cat = { meow: "purr", sound: "meow meow", age: 3 };
Ant.println("  cat['meow']:", cat["meow"]);
Ant.println("  cat['sound']:", cat["sound"]);
Ant.println("  cat['age']:", cat["age"]);
let key = "meow";
Ant.println("  cat[key] where key='meow':", cat[key]);

// Test 11: Dynamic property assignment with expressions
Ant.println("\nTest 11: Expression keys");
let obj11 = {};
obj11[1 + 1] = "two";
obj11[2 + 2] = "four";
Ant.println("  obj11[2]:", obj11[2]);
Ant.println("  obj11[4]:", obj11[4]);

// Test 12: Creating a map-like structure
Ant.println("\nTest 12: Map-like usage");
let map = {};
map["key1"] = { value: 10, type: "number" };
map["key2"] = { value: "hello", type: "string" };
map["key3"] = { value: true, type: "boolean" };
Ant.println("  map['key1'].value:", map["key1"]["value"]);
Ant.println("  map['key2'].value:", map["key2"]["value"]);
Ant.println("  map['key3'].value:", map["key3"]["value"]);

Ant.println("\n=== All tests completed ===");
