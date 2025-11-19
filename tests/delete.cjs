// Test delete keyword

Ant.println("=== Testing delete operator ===");

// Test 1: Delete object property
let obj = { a: 1, b: 2, c: 3 };
Ant.println(obj.a);  // 1
delete obj.a;
Ant.println(obj.a);  // undefined

// Test 2: Delete from nested object
let nested = {
  outer: {
    inner: "value"
  }
};
Ant.println(nested.outer.inner);  // "value"
delete nested.outer.inner;
Ant.println(nested.outer.inner);  // undefined

// Test 3: Delete array element
let arr = [1, 2, 3, 4, 5];
Ant.println(arr.length);  // 5
delete arr[2];
Ant.println(arr[2]);      // undefined
Ant.println(arr.length);  // 5 (length doesn't change)

// Test 4: Delete returns true
let test_obj = { x: 10 };
let result = delete test_obj.x;
Ant.println(result);  // true

// Test 5: Delete non-existent property
let obj2 = { a: 1 };
let result2 = delete obj2.b;  // Property doesn't exist
Ant.println(result2);  // true

// Test 6: Delete from variable (should return true but not delete var)
// Note: In this implementation, deleting a variable reference returns true
// but doesn't actually remove the variable from scope
let myVar = 42;
let result3 = delete myVar;
Ant.println(result3);  // true
// Ant.println(myVar);    // Would still be 42 if properly implemented

// Test 7: Cannot delete const property
const obj3 = { a: 1 };
// This would fail: delete obj3.a;  // Error: cannot delete constant property

// Test 8: Delete with dynamic object properties
let dynamicObj = {};
dynamicObj.prop1 = "first";
dynamicObj.prop2 = "second";
dynamicObj.prop3 = "third";

Ant.println(dynamicObj.prop1);  // "first"
Ant.println(dynamicObj.prop2);  // "second"

delete dynamicObj.prop2;
Ant.println(dynamicObj.prop1);  // "first"
Ant.println(dynamicObj.prop2);  // undefined
Ant.println(dynamicObj.prop3);  // "third"

// Test 9: Delete and re-add property
let reusableObj = { key: "value1" };
Ant.println(reusableObj.key);  // "value1"
delete reusableObj.key;
Ant.println(reusableObj.key);  // undefined
reusableObj.key = "value2";
Ant.println(reusableObj.key);  // "value2"

// Test 10: Delete in complex object structure
let complex = {
  level1: {
    level2: {
      level3: "deep"
    }
  }
};
Ant.println(complex.level1.level2.level3);  // "deep"
delete complex.level1.level2.level3;
Ant.println(complex.level1.level2.level3);  // undefined

Ant.println("=== Delete tests completed ===");
