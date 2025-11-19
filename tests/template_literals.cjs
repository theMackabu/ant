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

Ant.println("=== Template Literal Tests ===\n");

// Test 1: Basic template literal
Ant.println("Test 1: Basic template literal");
let basic = `Hello, World!`;
Ant.println("  Result:", basic);
Ant.println("  Type:", typeof basic);
Ant.println("  Length:", basic.length);

// Test 2: Single variable interpolation
Ant.println("\nTest 2: Single variable interpolation");
let name = "Alice";
let greeting = `Hello, ${name}!`;
Ant.println("  Name:", name);
Ant.println("  Result:", greeting);

// Test 3: Multiple variable interpolations
Ant.println("\nTest 3: Multiple variable interpolations");
let firstName = "John";
let lastName = "Doe";
let fullName = `${firstName} ${lastName}`;
Ant.println("  First:", firstName);
Ant.println("  Last:", lastName);
Ant.println("  Full:", fullName);

// Test 4: Numeric expressions
Ant.println("\nTest 4: Numeric expressions");
let a = 10;
let b = 20;
let sum = `${a} + ${b} = ${a + b}`;
let product = `${a} * ${b} = ${a * b}`;
let division = `${b} / ${a} = ${b / a}`;
Ant.println("  Sum:", sum);
Ant.println("  Product:", product);
Ant.println("  Division:", division);

// Test 5: Complex expressions
Ant.println("\nTest 5: Complex expressions");
let x = 5;
let y = 3;
let complex1 = `(${x} + ${y}) * 2 = ${(x + y) * 2}`;
let complex2 = `Average of ${x} and ${y} is ${(x + y) / 2}`;
Ant.println("  Complex 1:", complex1);
Ant.println("  Complex 2:", complex2);

// Test 6: Boolean interpolation
Ant.println("\nTest 6: Boolean interpolation");
let isActive = true;
let isDisabled = false;
let status1 = `Active: ${isActive}`;
let status2 = `Disabled: ${isDisabled}`;
Ant.println("  Status 1:", status1);
Ant.println("  Status 2:", status2);

// Test 7: Mixed types
Ant.println("\nTest 7: Mixed types");
let count = 42;
let label = "items";
let enabled = true;
let mixed = `Status: ${enabled}, Count: ${count} ${label}`;
Ant.println("  Result:", mixed);

// Test 8: Empty template
Ant.println("\nTest 8: Empty template");
let empty = ``;
Ant.println("  Result: '" + empty + "'");
Ant.println("  Length:", empty.length);

// Test 9: Template with only interpolation
Ant.println("\nTest 9: Template with only interpolation");
let value = 123;
let onlyInterp = `${value}`;
Ant.println("  Value:", value);
Ant.println("  Result:", onlyInterp);
Ant.println("  Type:", typeof onlyInterp);

// Test 10: Consecutive interpolations
Ant.println("\nTest 10: Consecutive interpolations");
let n1 = 1;
let n2 = 2;
let n3 = 3;
let consecutive = `${n1}${n2}${n3}`;
Ant.println("  Result:", consecutive);

// Test 11: String concatenation in interpolation
Ant.println("\nTest 11: String concatenation in interpolation");
let part1 = "Hello";
let part2 = "World";
let concatenated = `${part1 + " " + part2}`;
Ant.println("  Part 1:", part1);
Ant.println("  Part 2:", part2);
Ant.println("  Result:", concatenated);

// Test 12: Nested arithmetic
Ant.println("\nTest 12: Nested arithmetic");
let p = 3;
let q = 4;
let r = 5;
let nested = `${p} * ${q} + ${r} = ${p * q + r}`;
Ant.println("  Result:", nested);

// Test 13: Comparison in template
Ant.println("\nTest 13: Comparison in template");
let age = 20;
let canVote = `Age ${age}: Can vote = ${age >= 18}`;
Ant.println("  Result:", canVote);

// Test 14: Object property access
Ant.println("\nTest 14: Object property access");
let person = { name: "Bob", age: 25 };
let personInfo = `${person.name} is ${person.age} years old`;
Ant.println("  Result:", personInfo);

// Test 15: Array indexing
Ant.println("\nTest 15: Array indexing");
let colors = ["red", "green", "blue"];
let colorMsg = `First color: ${colors[0]}, Last: ${colors[2]}`;
Ant.println("  Result:", colorMsg);

// Test 16: Multiline template
Ant.println("\nTest 16: Multiline template");
let multiline = `Line 1
Line 2
Line 3`;
Ant.println("  Result:", multiline);
Ant.println("  Length:", multiline.length);

// Test 17: Escape sequences
Ant.println("\nTest 17: Escape sequences");
let escaped = `Hello\nWorld\tTab`;
Ant.println("  With escapes:", escaped);
let withBackslash = `Path: C:\\Users\\Data`;
Ant.println("  With backslash:", withBackslash);

// Test 18: Template in function
Ant.println("\nTest 18: Template in function");
function greet(name, time) {
  return `Good ${time}, ${name}!`;
}
let morning = greet("Alice", "morning");
let evening = greet("Bob", "evening");
Ant.println("  Morning:", morning);
Ant.println("  Evening:", evening);

// Test 19: Template with function call
Ant.println("\nTest 19: Template with function call");
function double(n) {
  return n * 2;
}
let funcResult = `Double of 5 is ${double(5)}`;
Ant.println("  Result:", funcResult);

// Test 20: Combining with string methods
Ant.println("\nTest 20: Combining with string methods");
let word = "javascript";
let upperFirst = word[0];
let rest = word.slice(1);
let capitalized = `${upperFirst}${rest}`;
Ant.println("  Original:", word);
Ant.println("  Capitalized:", capitalized);
Ant.println("  Includes 'script':", capitalized.includes("script"));

// Test 21: Price formatting example
Ant.println("\nTest 21: Price formatting example");
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
Ant.println(receipt);

// Test 22: URL building
Ant.println("\nTest 22: URL building");
let protocol = "https";
let domain = "example.com";
let path = "api/users";
let id = 123;
let url = `${protocol}://${domain}/${path}/${id}`;
Ant.println("  URL:", url);

// Test 23: Error message formatting
Ant.println("\nTest 23: Error message formatting");
let errorCode = 404;
let resource = "user";
let errorMsg = `Error ${errorCode}: ${resource} not found`;
Ant.println("  Error:", errorMsg);

// Test 24: Conditional expression in template
Ant.println("\nTest 24: Conditional expression in template");
let score = 85;
let grade = `Score: ${score} - ${score >= 90 ? "A" : score >= 80 ? "B" : "C"}`;
Ant.println("  Result:", grade);

// Test 25: Complex data formatting
Ant.println("\nTest 25: Complex data formatting");
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
Ant.println(userCard);

// Test 26: Edge case - very long interpolation
Ant.println("\nTest 26: Edge case - very long interpolation");
let longExpr = `Result: ${1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10}`;
Ant.println("  Result:", longExpr);

// Test 27: Template with null and undefined
Ant.println("\nTest 27: Template with null and undefined");
let nullVal = null;
let undefVal = undefined;
let nullMsg = `Null value: ${nullVal}`;
let undefMsg = `Undefined value: ${undefVal}`;
Ant.println("  Null:", nullMsg);
Ant.println("  Undefined:", undefMsg);

// Test 28: String with template-like syntax
Ant.println("\nTest 28: String with template-like syntax");
let innerCalc = 5 + 5;
let template = `Template with inner calculation: ${innerCalc}`;
Ant.println("  Result:", template);

// Test 29: Large numbers
Ant.println("\nTest 29: Large numbers");
let big = 1000000;
let formatted = `One million = ${big}`;
Ant.println("  Result:", formatted);

// Test 30: Performance test - multiple templates
Ant.println("\nTest 30: Performance test");
for (let i = 0; i < 5; i++) {
  let msg = `Iteration ${i}: ${i * 2}`;
  Ant.println("  " + msg);
}

Ant.println("\n=== All template literal tests completed ===");
