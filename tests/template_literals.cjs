// Template Literal Tests
// Testing backtick (`) strings and ${} interpolation
//
// This test suite validates the template literal functionality including:
// - Basic template literals with backticks
// - Variable interpolation with ${}
// - Expression evaluation within templates
// - Multiple and consecutive interpolations
// - Type conversion (numbers, booleans, objects)
// - Multiline strings
// - Escape sequences
// - Complex expressions and nested calculations
// - Integration with functions and methods
// - Edge cases (empty, null, undefined)
// - Practical use cases (URLs, receipts, error messages)

console.log("=== Template Literal Tests ===\n");

// Test 1: Basic template literal
console.log("Test 1: Basic template literal");
let basic = `Hello, World!`;
console.log("  Result:", basic);
console.log("  Type:", typeof basic);
console.log("  Length:", basic.length);

// Test 2: Single variable interpolation
console.log("\nTest 2: Single variable interpolation");
let name = "Alice";
let greeting = `Hello, ${name}!`;
console.log("  Name:", name);
console.log("  Result:", greeting);

// Test 3: Multiple variable interpolations
console.log("\nTest 3: Multiple variable interpolations");
let firstName = "John";
let lastName = "Doe";
let fullName = `${firstName} ${lastName}`;
console.log("  First:", firstName);
console.log("  Last:", lastName);
console.log("  Full:", fullName);

// Test 4: Numeric expressions
console.log("\nTest 4: Numeric expressions");
let a = 10;
let b = 20;
let sum = `${a} + ${b} = ${a + b}`;
let product = `${a} * ${b} = ${a * b}`;
let division = `${b} / ${a} = ${b / a}`;
console.log("  Sum:", sum);
console.log("  Product:", product);
console.log("  Division:", division);

// Test 5: Complex expressions
console.log("\nTest 5: Complex expressions");
let x = 5;
let y = 3;
let complex1 = `(${x} + ${y}) * 2 = ${(x + y) * 2}`;
let complex2 = `Average of ${x} and ${y} is ${(x + y) / 2}`;
console.log("  Complex 1:", complex1);
console.log("  Complex 2:", complex2);

// Test 6: Boolean interpolation
console.log("\nTest 6: Boolean interpolation");
let isActive = true;
let isDisabled = false;
let status1 = `Active: ${isActive}`;
let status2 = `Disabled: ${isDisabled}`;
console.log("  Status 1:", status1);
console.log("  Status 2:", status2);

// Test 7: Mixed types
console.log("\nTest 7: Mixed types");
let count = 42;
let label = "items";
let enabled = true;
let mixed = `Status: ${enabled}, Count: ${count} ${label}`;
console.log("  Result:", mixed);

// Test 8: Empty template
console.log("\nTest 8: Empty template");
let empty = ``;
console.log("  Result: '" + empty + "'");
console.log("  Length:", empty.length);

// Test 9: Template with only interpolation
console.log("\nTest 9: Template with only interpolation");
let value = 123;
let onlyInterp = `${value}`;
console.log("  Value:", value);
console.log("  Result:", onlyInterp);
console.log("  Type:", typeof onlyInterp);

// Test 10: Consecutive interpolations
console.log("\nTest 10: Consecutive interpolations");
let n1 = 1;
let n2 = 2;
let n3 = 3;
let consecutive = `${n1}${n2}${n3}`;
console.log("  Result:", consecutive);

// Test 11: String concatenation in interpolation
console.log("\nTest 11: String concatenation in interpolation");
let part1 = "Hello";
let part2 = "World";
let concatenated = `${part1 + " " + part2}`;
console.log("  Part 1:", part1);
console.log("  Part 2:", part2);
console.log("  Result:", concatenated);

// Test 12: Nested arithmetic
console.log("\nTest 12: Nested arithmetic");
let p = 3;
let q = 4;
let r = 5;
let nested = `${p} * ${q} + ${r} = ${p * q + r}`;
console.log("  Result:", nested);

// Test 13: Comparison in template
console.log("\nTest 13: Comparison in template");
let age = 20;
let canVote = `Age ${age}: Can vote = ${age >= 18}`;
console.log("  Result:", canVote);

// Test 14: Object property access
console.log("\nTest 14: Object property access");
let person = { name: "Bob", age: 25 };
let personInfo = `${person.name} is ${person.age} years old`;
console.log("  Result:", personInfo);

// Test 15: Array indexing
console.log("\nTest 15: Array indexing");
let colors = ["red", "green", "blue"];
let colorMsg = `First color: ${colors[0]}, Last: ${colors[2]}`;
console.log("  Result:", colorMsg);

// Test 16: Multiline template
console.log("\nTest 16: Multiline template");
let multiline = `Line 1
Line 2
Line 3`;
console.log("  Result:", multiline);
console.log("  Length:", multiline.length);

// Test 17: Escape sequences
console.log("\nTest 17: Escape sequences");
let escaped = `Hello\nWorld\tTab`;
console.log("  With escapes:", escaped);
let withBackslash = `Path: C:\\Users\\Data`;
console.log("  With backslash:", withBackslash);

// Test 18: Template in function
console.log("\nTest 18: Template in function");
function greet(name, time) {
  return `Good ${time}, ${name}!`;
}
let morning = greet("Alice", "morning");
let evening = greet("Bob", "evening");
console.log("  Morning:", morning);
console.log("  Evening:", evening);

// Test 19: Template with function call
console.log("\nTest 19: Template with function call");
function double(n) {
  return n * 2;
}
let funcResult = `Double of 5 is ${double(5)}`;
console.log("  Result:", funcResult);

// Test 20: Combining with string methods
console.log("\nTest 20: Combining with string methods");
let word = "javascript";
let upperFirst = word[0];
let rest = word.slice(1);
let capitalized = `${upperFirst}${rest}`;
console.log("  Original:", word);
console.log("  Capitalized:", capitalized);
console.log("  Includes 'script':", capitalized.includes("script"));

// Test 21: Price formatting example
console.log("\nTest 21: Price formatting example");
let price = 19.99;
let quantity = 3;
let tax = 0.08;
let subtotal = price * quantity;
let total = subtotal * (1 + tax);
let receipt = `
Item price: $${price}
Quantity: ${quantity}
Subtotal: $${subtotal}
Tax (${tax * 100}%): $${subtotal * tax}
Total: $${total}`;
console.log(receipt);

// Test 22: URL building
console.log("\nTest 22: URL building");
let protocol = "https";
let domain = "example.com";
let path = "api/users";
let id = 123;
let url = `${protocol}://${domain}/${path}/${id}`;
console.log("  URL:", url);

// Test 23: Error message formatting
console.log("\nTest 23: Error message formatting");
let errorCode = 404;
let resource = "user";
let errorMsg = `Error ${errorCode}: ${resource} not found`;
console.log("  Error:", errorMsg);

// Test 24: Conditional expression in template
console.log("\nTest 24: Conditional expression in template");
let score = 85;
let grade = `Score: ${score} - ${score >= 90 ? "A" : score >= 80 ? "B" : "C"}`;
console.log("  Result:", grade);

// Test 25: Complex data formatting
console.log("\nTest 25: Complex data formatting");
let user = {
  id: 1001,
  name: "Charlie",
  email: "charlie@example.com",
  active: true
};
let userCard = `
┌─────────────────────────
│ ID: ${user.id}
│ Name: ${user.name}
│ Email: ${user.email}
│ Status: ${user.active ? "Active" : "Inactive"}
└─────────────────────────`;
console.log(userCard);

// Test 26: Edge case - very long interpolation
console.log("\nTest 26: Edge case - very long interpolation");
let longExpr = `Result: ${1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10}`;
console.log("  Result:", longExpr);

// Test 27: Template with null and undefined
console.log("\nTest 27: Template with null and undefined");
let nullVal = null;
let undefVal = undefined;
let nullMsg = `Null value: ${nullVal}`;
let undefMsg = `Undefined value: ${undefVal}`;
console.log("  Null:", nullMsg);
console.log("  Undefined:", undefMsg);

// Test 28: String with template-like syntax
console.log("\nTest 28: String with template-like syntax");
let innerCalc = 5 + 5;
let template = `Template with inner calculation: ${innerCalc}`;
console.log("  Result:", template);

// Test 29: Large numbers
console.log("\nTest 29: Large numbers");
let big = 1000000;
let formatted = `One million = ${big}`;
console.log("  Result:", formatted);

// Test 30: Performance test - multiple templates
console.log("\nTest 30: Performance test");
for (let i = 0; i < 5; i++) {
  let msg = `Iteration ${i}: ${i * 2}`;
  console.log("  " + msg);
}

console.log("\n=== All template literal tests completed ===");
