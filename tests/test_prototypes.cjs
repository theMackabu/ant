// Test JavaScript Prototype Chain Implementation

console.log("=== Prototype Chain Tests ===\n");

// ============================================
// Test 1: Basic prototype chain for objects
// ============================================
console.log("--- Test 1: Object Prototypes ---");

let obj = {};
console.log("{} instanceof Object:", {} instanceof Object);
console.log("Object.getPrototypeOf({}) === Object.prototype:", 
  Object.getPrototypeOf({}) === Object.prototype);

// ============================================
// Test 2: Array prototype chain
// ============================================
console.log("\n--- Test 2: Array Prototypes ---");

let arr = [1, 2, 3];
console.log("[] instanceof Array:", [] instanceof Array);
console.log("[] instanceof Object:", [] instanceof Object);
console.log("Object.getPrototypeOf([]) === Array.prototype:", 
  Object.getPrototypeOf([]) === Array.prototype);

// Array prototype should chain to Object.prototype
let arrProto = Object.getPrototypeOf(arr);
let objProto = Object.getPrototypeOf(arrProto);
console.log("Array.prototype.__proto__ === Object.prototype:", 
  objProto === Object.prototype);

// ============================================
// Test 3: String prototype methods
// ============================================
console.log("\n--- Test 3: String Prototype Methods ---");

console.log("\"hello\".toUpperCase():", "hello".toUpperCase());
console.log("\"WORLD\".toLowerCase():", "WORLD".toLowerCase());
console.log("\"hello world\".split(\" \"):", "hello world".split(" "));
console.log("\"abc\".indexOf(\"b\"):", "abc".indexOf("b"));
console.log("\"  trimmed  \".trim():", "  trimmed  ".trim());
console.log("\"ab\".repeat(3):", "ab".repeat(3));
console.log("\"5\".padStart(3, \"0\"):", "5".padStart(3, "0"));

// ============================================
// Test 4: Number prototype methods
// ============================================
console.log("\n--- Test 4: Number Prototype Methods ---");

console.log("(3.14159).toFixed(2):", (3.14159).toFixed(2));
console.log("(42).toString():", (42).toString());
console.log("(123.456).toPrecision(4):", (123.456).toPrecision(4));

// ============================================
// Test 5: Array prototype methods
// ============================================
console.log("\n--- Test 5: Array Prototype Methods ---");

let testArr = [1, 2, 3];
console.log("Initial array:", testArr);
console.log("[1,2,3].push(4):", testArr.push(4));
console.log("After push:", testArr);
console.log("[1,2,3,4].pop():", testArr.pop());
console.log("After pop:", testArr);
console.log("[1,2,3].slice(1):", [1,2,3].slice(1));
console.log("[1,2,3].join(\"-\"):", [1,2,3].join("-"));
console.log("[1,2,3].includes(2):", [1,2,3].includes(2));

// ============================================
// Test 6: Object.create with prototype
// ============================================
console.log("\n--- Test 6: Object.create ---");

let animal = { 
  speak: function() { return "animal sound"; },
  type: "animal"
};
let dog = Object.create(animal);
dog.bark = function() { return "woof"; };
dog.name = "Rex";

console.log("dog.speak() (inherited):", dog.speak());
console.log("dog.bark() (own):", dog.bark());
console.log("dog.type (inherited):", dog.type);
console.log("dog.name (own):", dog.name);
console.log("\"speak\" in dog:", "speak" in dog);
console.log("\"bark\" in dog:", "bark" in dog);
console.log("Object.hasOwn(dog, \"speak\"):", Object.hasOwn(dog, "speak"));
console.log("Object.hasOwn(dog, \"bark\"):", Object.hasOwn(dog, "bark"));

// ============================================
// Test 7: Object.setPrototypeOf
// ============================================
console.log("\n--- Test 7: Object.setPrototypeOf ---");

let target = { own: "property" };
let proto = { inherited: "value", method: function() { return "from proto"; } };
Object.setPrototypeOf(target, proto);

console.log("target.own:", target.own);
console.log("target.inherited:", target.inherited);
console.log("target.method():", target.method());
console.log("Object.getPrototypeOf(target) === proto:", Object.getPrototypeOf(target) === proto);

// ============================================
// Test 8: instanceof with prototype chain
// ============================================
console.log("\n--- Test 8: instanceof ---");

console.log("{} instanceof Object:", {} instanceof Object);
console.log("[] instanceof Array:", [] instanceof Array);
console.log("[] instanceof Object:", [] instanceof Object);
console.log("\"str\" instanceof String:", "str" instanceof String);
console.log("42 instanceof Number:", 42 instanceof Number);
console.log("true instanceof Boolean:", true instanceof Boolean);

// Custom prototype chain
let customProto = {};
let customObj = Object.create(customProto);
console.log("Object.getPrototypeOf(customObj) === customProto:", 
  Object.getPrototypeOf(customObj) === customProto);

// ============================================
// Test 9: 'in' operator with prototype chain
// ============================================
console.log("\n--- Test 9: 'in' Operator ---");

let base = { baseMethod: function() {} };
let derived = Object.create(base);
derived.derivedMethod = function() {};

console.log("\"baseMethod\" in derived:", "baseMethod" in derived);
console.log("\"derivedMethod\" in derived:", "derivedMethod" in derived);
console.log("\"nonexistent\" in derived:", "nonexistent" in derived);

// Array methods should be found via prototype
console.log("\"push\" in []:", "push" in []);
console.log("\"pop\" in []:", "pop" in []);
console.log("\"slice\" in []:", "slice" in []);

// ============================================
// Test 10: Property shadowing with new properties
// ============================================
console.log("\n--- Test 10: Property Shadowing ---");

let parent = { value: "parent", shared: "from parent" };
let child = Object.create(parent);
// Note: To shadow a prototype property, declare it as a new own property
child.childOnly = "child's own";

console.log("parent.value:", parent.value);
console.log("child.value (inherited):", child.value);
console.log("child.shared (inherited):", child.shared);
console.log("child.childOnly (own):", child.childOnly);
console.log("Object.hasOwn(child, \"childOnly\"):", Object.hasOwn(child, "childOnly"));
console.log("Object.hasOwn(child, \"value\"):", Object.hasOwn(child, "value"));

// ============================================
// Test 11: Object.keys doesn't include prototype props
// ============================================
console.log("\n--- Test 11: Object.keys ---");

let protoObj = { protoKey: 1 };
let ownObj = Object.create(protoObj);
ownObj.ownKey = 2;

let keys = Object.keys(ownObj);
console.log("Object.keys(ownObj):", keys);
console.log("Includes ownKey:", keys.includes("ownKey"));
console.log("Includes protoKey:", keys.includes("protoKey"));

// ============================================
// Test 12: Constructor.prototype
// ============================================
console.log("\n--- Test 12: Constructor.prototype ---");

console.log("Array.prototype exists:", Array.prototype !== undefined);
console.log("String.prototype exists:", String.prototype !== undefined);
console.log("Number.prototype exists:", Number.prototype !== undefined);
console.log("Object.prototype exists:", Object.prototype !== undefined);
console.log("Function.prototype exists:", Function.prototype !== undefined);

console.log("\n=== All Prototype Tests Complete ===");
