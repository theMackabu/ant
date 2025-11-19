// Test equality operators == and !=
// Also tests strict equality === and !==

Ant.println("=== Equality Operators Test ===\n");

// Test 1: Basic equality with numbers
Ant.println("Test 1: Number equality");
let a = 5;
let b = 5;
let c = 10;

Ant.println("  5 == 5:", a == b);
Ant.println("  5 === 5:", a === b);
Ant.println("  5 != 10:", a != c);
Ant.println("  5 !== 10:", a !== c);
Ant.println("  5 == 10:", a == c);
Ant.println("  5 != 5:", a != b);

// Test 2: String equality
Ant.println("\nTest 2: String equality");
let s1 = "hello";
let s2 = "hello";
let s3 = "world";

Ant.println("  'hello' == 'hello':", s1 == s2);
Ant.println("  'hello' === 'hello':", s1 === s2);
Ant.println("  'hello' != 'world':", s1 != s3);
Ant.println("  'hello' !== 'world':", s1 !== s3);
Ant.println("  'hello' == 'world':", s1 == s3);

// Test 3: Boolean equality
Ant.println("\nTest 3: Boolean equality");
let t1 = true;
let t2 = true;
let f1 = false;

Ant.println("  true == true:", t1 == t2);
Ant.println("  true === true:", t1 === t2);
Ant.println("  true != false:", t1 != f1);
Ant.println("  true !== false:", t1 !== f1);
Ant.println("  false == false:", f1 == false);

// Test 4: Undefined and null
Ant.println("\nTest 4: Undefined and null");
let u = undefined;
let n = null;

Ant.println("  undefined == undefined:", u == undefined);
Ant.println("  undefined === undefined:", u === undefined);
Ant.println("  null == null:", n == null);
Ant.println("  null === null:", n === null);
Ant.println("  undefined != null:", u != n);
Ant.println("  undefined !== null:", u !== n);

// Test 5: Mixed type comparisons
Ant.println("\nTest 5: Type comparisons");
let num = 5;
let str = "5";
let bool = true;

Ant.println("  5 != '5':", num != str);
Ant.println("  5 !== '5':", num !== str);
Ant.println("  true != 1:", bool != 1);
Ant.println("  true !== 1:", bool !== 1);

// Test 6: In conditional expressions
Ant.println("\nTest 6: Conditional expressions");
if (5 == 5) {
  Ant.println("  5 == 5 in if statement: passed");
}

if (5 != 10) {
  Ant.println("  5 != 10 in if statement: passed");
}

if (undefined == undefined) {
  Ant.println("  undefined == undefined in if statement: passed");
}

if (null != undefined) {
  Ant.println("  null != undefined in if statement: passed");
}

// Test 7: Loop conditions
Ant.println("\nTest 7: Loop with equality");
let count = 0;
for (let i = 0; i != 5; i = i + 1) {
  count = count + 1;
}
Ant.println("  Loop with != condition, count:", count);

// Test 8: Ternary with equality
Ant.println("\nTest 8: Ternary operator");
let result1 = (5 == 5) ? "equal" : "not equal";
Ant.println("  5 == 5 ? 'equal' : 'not equal' =>", result1);

let result2 = (5 != 10) ? "not equal" : "equal";
Ant.println("  5 != 10 ? 'not equal' : 'equal' =>", result2);

// Test 9: Object equality (by reference)
Ant.println("\nTest 9: Object equality");
let obj1 = { x: 1 };
let obj2 = { x: 1 };
let obj3 = obj1;

Ant.println("  obj1 == obj2 (different refs):", obj1 == obj2);
Ant.println("  obj1 === obj2 (different refs):", obj1 === obj2);
Ant.println("  obj1 == obj3 (same ref):", obj1 == obj3);
Ant.println("  obj1 === obj3 (same ref):", obj1 === obj3);
Ant.println("  obj1 != obj2:", obj1 != obj2);

// Test 10: Array equality
Ant.println("\nTest 10: Array equality");
let arr1 = [1, 2, 3];
let arr2 = [1, 2, 3];
let arr3 = arr1;

Ant.println("  arr1 == arr2 (different refs):", arr1 == arr2);
Ant.println("  arr1 === arr2 (different refs):", arr1 === arr2);
Ant.println("  arr1 == arr3 (same ref):", arr1 == arr3);
Ant.println("  arr1 === arr3 (same ref):", arr1 === arr3);

Ant.println("\n=== All tests completed ===");
