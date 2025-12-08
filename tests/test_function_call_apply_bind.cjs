// Test Function.prototype.call, apply, and bind methods

// Test 1: Basic call with no this
function greet(name) { return "Hello, " + name; }
console.log("Test 1 - call:", greet.call(null, "World")); // Hello, World

// Test 2: call with custom this
function showThis() { return this.value; }
let obj = { value: 42 };
console.log("Test 2 - call with this:", showThis.call(obj)); // 42

// Test 3: call with multiple arguments
function add(a, b, c) { return a + b + c; }
console.log("Test 3 - call multiple args:", add.call(null, 1, 2, 3)); // 6

// Test 4: apply with array of arguments
function sum(a, b, c) { return a + b + c; }
console.log("Test 4 - apply:", sum.apply(null, [10, 20, 30])); // 60

// Test 5: apply with custom this
function getInfo() { return this.name + " is " + this.age; }
let person = { name: "Alice", age: 30 };
console.log("Test 5 - apply with this:", getInfo.apply(person, [])); // Alice is 30

// Test 6: bind creates new function with bound this
function getValue() { return this.x; }
let data = { x: 100 };
let boundFn = getValue.bind(data);
console.log("Test 6 - bind:", boundFn()); // 100

// Test 7: bound function ignores call-site this
let other = { x: 999 };
other.fn = boundFn;
console.log("Test 7 - bound ignores new this:", other.fn()); // 100

// Test 8: call with undefined this (should work)
function returnArg(x) { return x * 2; }
console.log("Test 8 - call undefined this:", returnArg.call(undefined, 5)); // 10

// Test 9: Method borrowing with call
let arr1 = { 0: "a", 1: "b", length: 2 };
function joinItems() {
  let result = "";
  for (let i = 0; i < this.length; i++) {
    if (i > 0) result = result + ",";
    result = result + this[i];
  }
  return result;
}
console.log("Test 9 - method borrowing:", joinItems.call(arr1)); // a,b

// Test 10: Chained function operations
function multiply(factor) { return this.base * factor; }
let calc = { base: 5 };
console.log("Test 10 - chained:", multiply.call(calc, 3)); // 15

console.log("\nAll Function.prototype.call/apply/bind tests completed!");
