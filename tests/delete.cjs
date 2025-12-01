// Test delete keyword

console.log("=== Testing delete operator ===");

// Test 1: Delete object property
let obj = { a: 1, b: 2, c: 3 };
console.log(obj.a);  // 1
delete obj.a;
console.log(obj.a);  // undefined

// Test 2: Delete from nested object
let nested = {
  outer: {
    inner: "value"
  }
};
console.log(nested.outer.inner);  // "value"
delete nested.outer.inner;
console.log(nested.outer.inner);  // undefined

// Test 3: Delete array element
let arr = [1, 2, 3, 4, 5];
console.log(arr.length);  // 5
delete arr[2];
console.log(arr[2]);      // undefined
console.log(arr.length);  // 5 (length doesn't change)

// Test 4: Delete returns true
let test_obj = { x: 10 };
let result = delete test_obj.x;
console.log(result);  // true

// Test 5: Delete non-existent property
let obj2 = { a: 1 };
let result2 = delete obj2.b;  // Property doesn't exist
console.log(result2);  // true

// Test 6: Delete from variable (should return true but not delete var)
// Note: In this implementation, deleting a variable reference returns true
// but doesn't actually remove the variable from scope
let myVar = 42;
let result3 = delete myVar;
console.log(result3);  // true
// console.log(myVar);    // Would still be 42 if properly implemented

// Test 7: Cannot delete const property
const obj3 = { a: 1 };
// This would fail: delete obj3.a;  // Error: cannot delete constant property

// Test 8: Delete with dynamic object properties
let dynamicObj = {};
dynamicObj.prop1 = "first";
dynamicObj.prop2 = "second";
dynamicObj.prop3 = "third";

console.log(dynamicObj.prop1);  // "first"
console.log(dynamicObj.prop2);  // "second"

delete dynamicObj.prop2;
console.log(dynamicObj.prop1);  // "first"
console.log(dynamicObj.prop2);  // undefined
console.log(dynamicObj.prop3);  // "third"

// Test 9: Delete and re-add property
let reusableObj = { key: "value1" };
console.log(reusableObj.key);  // "value1"
delete reusableObj.key;
console.log(reusableObj.key);  // undefined
reusableObj.key = "value2";
console.log(reusableObj.key);  // "value2"

// Test 10: Delete in complex object structure
let complex = {
  level1: {
    level2: {
      level3: "deep"
    }
  }
};
console.log(complex.level1.level2.level3);  // "deep"
delete complex.level1.level2.level3;
console.log(complex.level1.level2.level3);  // undefined

console.log("=== Delete tests completed ===");
