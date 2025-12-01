// Test equality operators == and !=
// Also tests strict equality === and !==

console.log("=== Equality Operators Test ===\n");

// Test 1: Basic equality with numbers
console.log("Test 1: Number equality");
let a = 5;
let b = 5;
let c = 10;

console.log("  5 == 5:", a == b);
console.log("  5 === 5:", a === b);
console.log("  5 != 10:", a != c);
console.log("  5 !== 10:", a !== c);
console.log("  5 == 10:", a == c);
console.log("  5 != 5:", a != b);

// Test 2: String equality
console.log("\nTest 2: String equality");
let s1 = "hello";
let s2 = "hello";
let s3 = "world";

console.log("  'hello' == 'hello':", s1 == s2);
console.log("  'hello' === 'hello':", s1 === s2);
console.log("  'hello' != 'world':", s1 != s3);
console.log("  'hello' !== 'world':", s1 !== s3);
console.log("  'hello' == 'world':", s1 == s3);

// Test 3: Boolean equality
console.log("\nTest 3: Boolean equality");
let t1 = true;
let t2 = true;
let f1 = false;

console.log("  true == true:", t1 == t2);
console.log("  true === true:", t1 === t2);
console.log("  true != false:", t1 != f1);
console.log("  true !== false:", t1 !== f1);
console.log("  false == false:", f1 == false);

// Test 4: Undefined and null
console.log("\nTest 4: Undefined and null");
let u = undefined;
let n = null;

console.log("  undefined == undefined:", u == undefined);
console.log("  undefined === undefined:", u === undefined);
console.log("  null == null:", n == null);
console.log("  null === null:", n === null);
console.log("  undefined != null:", u != n);
console.log("  undefined !== null:", u !== n);

// Test 5: Mixed type comparisons
console.log("\nTest 5: Type comparisons");
let num = 5;
let str = "5";
let bool = true;

console.log("  5 != '5':", num != str);
console.log("  5 !== '5':", num !== str);
console.log("  true != 1:", bool != 1);
console.log("  true !== 1:", bool !== 1);

// Test 6: In conditional expressions
console.log("\nTest 6: Conditional expressions");
if (5 == 5) {
  console.log("  5 == 5 in if statement: passed");
}

if (5 != 10) {
  console.log("  5 != 10 in if statement: passed");
}

if (undefined == undefined) {
  console.log("  undefined == undefined in if statement: passed");
}

if (null != undefined) {
  console.log("  null != undefined in if statement: passed");
}

// Test 7: Loop conditions
console.log("\nTest 7: Loop with equality");
let count = 0;
for (let i = 0; i != 5; i = i + 1) {
  count = count + 1;
}
console.log("  Loop with != condition, count:", count);

// Test 8: Ternary with equality
console.log("\nTest 8: Ternary operator");
let result1 = (5 == 5) ? "equal" : "not equal";
console.log("  5 == 5 ? 'equal' : 'not equal' =>", result1);

let result2 = (5 != 10) ? "not equal" : "equal";
console.log("  5 != 10 ? 'not equal' : 'equal' =>", result2);

// Test 9: Object equality (by reference)
console.log("\nTest 9: Object equality");
let obj1 = { x: 1 };
let obj2 = { x: 1 };
let obj3 = obj1;

console.log("  obj1 == obj2 (different refs):", obj1 == obj2);
console.log("  obj1 === obj2 (different refs):", obj1 === obj2);
console.log("  obj1 == obj3 (same ref):", obj1 == obj3);
console.log("  obj1 === obj3 (same ref):", obj1 === obj3);
console.log("  obj1 != obj2:", obj1 != obj2);

// Test 10: Array equality
console.log("\nTest 10: Array equality");
let arr1 = [1, 2, 3];
let arr2 = [1, 2, 3];
let arr3 = arr1;

console.log("  arr1 == arr2 (different refs):", arr1 == arr2);
console.log("  arr1 === arr2 (different refs):", arr1 === arr2);
console.log("  arr1 == arr3 (same ref):", arr1 == arr3);
console.log("  arr1 === arr3 (same ref):", arr1 === arr3);

console.log("\n=== All tests completed ===");
