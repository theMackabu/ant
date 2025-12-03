// Test ternary operator (?:), logical OR (||), and nullish coalescing (??)

console.log("=== Ternary, Logical OR, and Nullish Coalescing Test ===\n");

// Test 1: Basic ternary operator
console.log("Test 1: Basic ternary operator");
let x = 10;
let y = 5;

let result1 = x > y ? "x is greater" : "y is greater or equal";
console.log("  10 > 5 ? 'x is greater' : 'y is greater or equal' =>", result1);

let result2 = x < y ? "x is less" : "x is greater or equal";
console.log("  10 < 5 ? 'x is less' : 'x is greater or equal' =>", result2);

let result3 = true ? "yes" : "no";
console.log("  true ? 'yes' : 'no' =>", result3);

let result4 = false ? "yes" : "no";
console.log("  false ? 'yes' : 'no' =>", result4);

// Test 2: Nested ternary operators
console.log("\nTest 2: Nested ternary operators");
let age = 25;
let category = age < 18 ? "minor" : age < 65 ? "adult" : "senior";
console.log("  age = 25, category:", category);

age = 15;
category = age < 18 ? "minor" : age < 65 ? "adult" : "senior";
console.log("  age = 15, category:", category);

age = 70;
category = age < 18 ? "minor" : age < 65 ? "adult" : "senior";
console.log("  age = 70, category:", category);

// Test 3: Ternary with various types
console.log("\nTest 3: Ternary with various types");
let score = 85;
let grade = score >= 90 ? "A" : score >= 80 ? "B" : score >= 70 ? "C" : "F";
console.log("  score = 85, grade:", grade);

let hasPermission = true;
let status = hasPermission ? 200 : 403;
console.log("  hasPermission = true, status:", status);

let value = null;
let output = value ? value : "default";
console.log("  null ? null : 'default' =>", output);

// Test 4: Logical OR (||) operator
console.log("\nTest 4: Logical OR (||) operator");

// With boolean values
let bool1 = true || false;
let bool2 = false || true;
let bool3 = false || false;
let bool4 = true || true;
console.log("  true || false =>", bool1);
console.log("  false || true =>", bool2);
console.log("  false || false =>", bool3);
console.log("  true || true =>", bool4);

// Test 5: Logical OR with truthy/falsy values
console.log("\nTest 5: Logical OR with truthy/falsy values");
let val1 = null || "default";
let val2 = undefined || "fallback";
let val3 = "" || "empty string fallback";
let val4 = 0 || 42;
let val5 = false || true;
let val6 = "first" || "second";
let val7 = 100 || 200;

console.log("  null || 'default' =>", val1);
console.log("  undefined || 'fallback' =>", val2);
console.log("  '' || 'empty string fallback' =>", val3);
console.log("  0 || 42 =>", val4);
console.log("  false || true =>", val5);
console.log("  'first' || 'second' =>", val6);
console.log("  100 || 200 =>", val7);

// Test 6: Chained logical OR
console.log("\nTest 6: Chained logical OR");
let option1 = null;
let option2 = undefined;
let option3 = "";
let option4 = "valid value";
let finalValue = option1 || option2 || option3 || option4 || "ultimate fallback";
console.log("  null || undefined || '' || 'valid value' || 'ultimate fallback' =>", finalValue);

let allFalsy = null || undefined || false || 0 || "";
console.log("  null || undefined || false || 0 || '' =>", allFalsy);

// Test 7: Nullish coalescing (??) operator
console.log("\nTest 7: Nullish coalescing (??) operator");

// Basic nullish coalescing
let nc1 = null ?? "default";
let nc2 = undefined ?? "fallback";
let nc3 = "" ?? "empty string fallback";
let nc4 = 0 ?? 42;
let nc5 = false ?? true;
let nc6 = "value" ?? "default";

console.log("  null ?? 'default' =>", nc1);
console.log("  undefined ?? 'fallback' =>", nc2);
console.log("  '' ?? 'empty string fallback' =>", nc3);
console.log("  0 ?? 42 =>", nc4);
console.log("  false ?? true =>", nc5);
console.log("  'value' ?? 'default' =>", nc6);

// Test 8: Difference between || and ??
console.log("\nTest 8: Difference between || and ??");
console.log("  Comparing || and ?? with various values:");

let testVal = 0;
console.log("  0 || 'fallback' =>", testVal || "fallback");
console.log("  0 ?? 'fallback' =>", testVal ?? "fallback");

testVal = "";
console.log("  '' || 'fallback' =>", testVal || "fallback");
console.log("  '' ?? 'fallback' =>", testVal ?? "fallback");

testVal = false;
console.log("  false || 'fallback' =>", testVal || "fallback");
console.log("  false ?? 'fallback' =>", testVal ?? "fallback");

testVal = null;
console.log("  null || 'fallback' =>", testVal || "fallback");
console.log("  null ?? 'fallback' =>", testVal ?? "fallback");

testVal = undefined;
console.log("  undefined || 'fallback' =>", testVal || "fallback");
console.log("  undefined ?? 'fallback' =>", testVal ?? "fallback");

// Test 9: Chained nullish coalescing
console.log("\nTest 9: Chained nullish coalescing");
let config1 = null;
let config2 = undefined;
let config3 = "configured";
let config = config1 ?? config2 ?? config3 ?? "default config";
console.log("  null ?? undefined ?? 'configured' ?? 'default config' =>", config);

let allNull = null ?? undefined ?? null;
console.log("  null ?? undefined ?? null =>", allNull);

// Test 10: Ternary with || and ??
console.log("\nTest 10: Combining ternary with || and ??");
let username = null;
let displayName = username ? username : "Guest";
console.log("  username = null, ternary result:", displayName);

displayName = username || "Guest (using ||)";
console.log("  username = null, || result:", displayName);

displayName = username ?? "Guest (using ??)";
console.log("  username = null, ?? result:", displayName);

username = "";
displayName = username || "Guest (using ||)";
console.log("  username = '', || result:", displayName);

displayName = username ?? "Guest (using ??)";
console.log("  username = '', ?? result:", displayName);

// Test 11: In conditional expressions
console.log("\nTest 11: In conditional expressions");
let a = 5;
let b = 10;

if (a > b ? true : false) {
    console.log("  a > b: true");
} else {
    console.log("  a > b: false");
}

let condition = null || undefined || "has value";
if (condition) {
    console.log("  Condition with || is truthy:", condition);
}

let ncCondition = null ?? undefined ?? "has value";
if (ncCondition) {
    console.log("  Condition with ?? is truthy:", ncCondition);
}

// Test 12: In function returns
console.log("\nTest 12: In function returns");
function getMax(x, y) {
    return x > y ? x : y;
}

console.log("  getMax(15, 20):", getMax(15, 20));
console.log("  getMax(30, 10):", getMax(30, 10));

function getFirstValid(a, b, c) {
    return a || b || c || "none";
}

console.log("  getFirstValid(null, '', 'value'):", getFirstValid(null, "", "value"));
console.log("  getFirstValid(null, undefined, false):", getFirstValid(null, undefined, false));

function getFirstNonNullish(a, b, c) {
    return a ?? b ?? c ?? "none";
}

console.log("  getFirstNonNullish(null, '', 'value'):", getFirstNonNullish(null, "", "value"));
console.log("  getFirstNonNullish(null, undefined, false):", getFirstNonNullish(null, undefined, false));

// Test 13: Assignment with operators
console.log("\nTest 13: Assignment with operators");
let setting = null;
let finalSetting = setting ?? "default";
console.log("  setting = null, finalSetting:", finalSetting);

setting = undefined;
finalSetting = setting ?? "default";
console.log("  setting = undefined, finalSetting:", finalSetting);

setting = 0;
finalSetting = setting ?? "default";
console.log("  setting = 0, finalSetting:", finalSetting);

// Test 14: With objects and arrays
console.log("\nTest 14: With objects and arrays");
let user = null;
let defaultUser = { name: "Guest", id: 0 };
let currentUser = user ?? defaultUser;
console.log("  user = null, currentUser:", currentUser);

let items = null;
let defaultItems = [1, 2, 3];
let currentItems = items ?? defaultItems;
console.log("  items = null, currentItems:", currentItems);

let emptyArray = [];
let arrayResult = emptyArray || [1, 2, 3];
console.log("  [] || [1,2,3] =>", arrayResult);

arrayResult = emptyArray ?? [1, 2, 3];
console.log("  [] ?? [1,2,3] =>", arrayResult);

// Test 15: Complex expressions
console.log("\nTest 15: Complex expressions");
let min = 10;
let max = 100;
let input = 50;
let clamped = input < min ? min : input > max ? max : input;
console.log("  Clamping 50 to [10, 100]:", clamped);

input = 5;
clamped = input < min ? min : input > max ? max : input;
console.log("  Clamping 5 to [10, 100]:", clamped);

input = 150;
clamped = input < min ? min : input > max ? max : input;
console.log("  Clamping 150 to [10, 100]:", clamped);

let priority = null;
let defaultPriority = 5;
let finalPriority = (priority ?? defaultPriority) > 10 ? 10 : (priority ?? defaultPriority);
console.log("  priority = null, clamped to max 10:", finalPriority);

console.log("\n=== All tests completed ===");
