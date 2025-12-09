// Test edge cases for Object.defineProperty()

console.log("Test 1: Error when called with < 3 arguments");
try {
  Object.defineProperty({}, "foo");
  console.log("FAIL: Should have thrown error");
} catch (e) {
  console.log("PASS: Correctly throws error");
}

console.log("\nTest 2: Error when first argument is not an object");
try {
  Object.defineProperty(42, "foo", { value: 1 });
  console.log("FAIL: Should have thrown error");
} catch (e) {
  console.log("PASS: Correctly throws error for non-object");
}

console.log("\nTest 3: Error when descriptor is not an object");
try {
  Object.defineProperty({}, "foo", "not an object");
  console.log("FAIL: Should have thrown error");
} catch (e) {
  console.log("PASS: Correctly throws error for non-object descriptor");
}

console.log("\nTest 4: Error when mixing data and accessor descriptors");
try {
  Object.defineProperty({}, "foo", {
    value: 42,
    get: function() { return 0; }
  });
  console.log("FAIL: Should have thrown error");
} catch (e) {
  console.log("PASS: Correctly throws error for mixed descriptors");
}

console.log("\nTest 5: Can define property with just value");
const obj5 = {};
Object.defineProperty(obj5, "test", { value: 100 });
console.log("PASS: Property defined with just value:", obj5.test);

console.log("\nTest 6: Cannot define __proto__ property");
try {
  Object.defineProperty({}, "__proto__", { value: {} });
  console.log("FAIL: Should have thrown error");
} catch (e) {
  console.log("PASS: Correctly prevents __proto__ definition");
}

console.log("\nTest 7: Works with arrays");
const arr = [1, 2, 3];
Object.defineProperty(arr, "myProp", { value: "test" });
console.log("PASS: Works with arrays:", arr.myProp);

console.log("\nTest 8: Works with functions");
function myFunc() {}
Object.defineProperty(myFunc, "customProp", { value: 42 });
console.log("PASS: Works with functions:", myFunc.customProp);

console.log("\nAll edge case tests completed!");
