// Comprehensive Tagged Template Literal Tests
console.log("=== Tagged Template Literal Tests ===\n");

// Test 1: Basic tagged template with one substitution
console.log("Test 1: Basic tagged template");
function tag1(strings, ...values) {
  console.log("  ✓ strings.length:", strings.length);
  console.log("  ✓ strings[0]:", strings[0]);
  console.log("  ✓ strings[1]:", strings[1]);
  console.log("  ✓ values.length:", values.length);
  console.log("  ✓ values[0]:", values[0]);
  return "result1";
}

let name = "World";
let result1 = tag1`Hello ${name}!`;
console.log("  ✓ Result:", result1);
console.log("");

// Test 2: Building a string from parts
console.log("Test 2: Building a string from tagged template");
function build(strings, ...values) {
  let result = "";
  for (let i = 0; i < strings.length; i++) {
    result = result + strings[i];
    if (i < values.length) {
      result = result + values[i];
    }
  }
  return result;
}

let greeting = build`Hello ${name}!`;
console.log("  ✓ Result:", greeting);
console.log("");

// Test 3: Multiple substitutions
console.log("Test 3: Multiple substitutions");
function multi(strings, ...values) {
  console.log("  ✓ values.length:", values.length);
  console.log("  ✓ First value:", values[0]);
  console.log("  ✓ Second value:", values[1]);
  return values[0] + values[1];
}

let a = 10;
let b = 20;
let sum = multi`Adding ${a} and ${b}`;
console.log("  ✓ Sum:", sum);
console.log("");

// Test 4: No substitutions
console.log("Test 4: No substitutions");
function noSub(strings) {
  console.log("  ✓ strings.length:", strings.length);
  console.log("  ✓ strings[0]:", strings[0]);
  return "no-substitutions";
}

let result4 = noSub`Just a plain string`;
console.log("  ✓ Result:", result4);
console.log("");

// Test 5: Expression in substitution
console.log("Test 5: Expression in substitution");
function expr(strings, ...values) {
  return values[0] * 2;
}

let x = 5;
let doubled = expr`Double ${x + 3}`;
console.log("  ✓ Result:", doubled);
console.log("");

// Test 6: Conditional in substitution
console.log("Test 6: Conditional in substitution");
function cond(strings, ...values) {
  return values[0];
}

let score = 85;
let grade = cond`Grade: ${score > 80 ? "A" : "B"}`;
console.log("  ✓ Result:", grade);
console.log("");

// Test 7: Object property access
console.log("Test 7: Object property access");
function objAccess(strings, ...values) {
  return values[0];
}

let person = { name: "Alice", age: 25 };
let info = objAccess`Name: ${person.name}`;
console.log("  ✓ Result:", info);
console.log("");

// Test 8: Array indexing
console.log("Test 8: Array indexing");
function arrAccess(strings, ...values) {
  return values[0];
}

let colors = ["red", "green", "blue"];
let color = arrAccess`First color: ${colors[0]}`;
console.log("  ✓ Result:", color);
console.log("");

// Test 9: Returning different types
console.log("Test 9: Returning different types");
function retNum(strings) {
  return 42;
}

function retBool(strings) {
  return true;
}

let num = retNum`test`;
let bool = retBool`test`;
console.log("  ✓ Number:", num);
console.log("  ✓ Boolean:", bool);
console.log("");

// Test 10: Using result with string methods
console.log("Test 10: Using result with string methods");
function makeString(strings, ...values) {
  return strings[0] + values[0] + strings[1];
}

let text = makeString`Hello ${name}!`;
console.log("  ✓ Text:", text);
console.log("  ✓ Includes 'World':", text.includes("World"));
console.log("  ✓ Starts with 'Hello':", text.startsWith("Hello"));
console.log("");

// Test 11: Rest params with many values
console.log("Test 11: Rest params with many values");
function manyValues(strings, ...values) {
  console.log("  ✓ strings.length:", strings.length);
  console.log("  ✓ values.length:", values.length);
  let result = "";
  for (let i = 0; i < strings.length; i++) {
    result = result + strings[i];
    if (i < values.length) {
      result = result + values[i];
    }
  }
  return result;
}

let v1 = "A";
let v2 = "B";
let v3 = "C";
let v4 = "D";
let assembled = manyValues`${v1}-${v2}-${v3}-${v4}`;
console.log("  ✓ Result:", assembled);
console.log("");

// Test 12: Nested function calls
console.log("Test 12: Nested expressions");
function nested(strings, ...values) {
  return values[0] + values[1];
}

function double(n) {
  return n * 2;
}

let nested_result = nested`${double(5)} + ${double(10)}`;
console.log("  ✓ Result:", nested_result);
console.log("");

// Test 13: Empty strings between substitutions
console.log("Test 13: Adjacent substitutions");
function adjacent(strings, ...values) {
  console.log("  ✓ strings:", strings);
  console.log("  ✓ values:", values);
  return values[0] + values[1];
}

let adj = adjacent`${10}${20}`;
console.log("  ✓ Result:", adj);
console.log("");

// Test 14: SQL-style template (common use case)
console.log("Test 14: SQL-style builder");
function sql(strings, ...values) {
  let query = "";
  for (let i = 0; i < strings.length; i++) {
    query = query + strings[i];
    if (i < values.length) {
      query = query + "$" + (i + 1);
    }
  }
  return { query: query, values: values };
}

let userId = 42;
let userName = "Alice";
let sqlResult = sql`SELECT * FROM users WHERE id = ${userId} AND name = ${userName}`;
console.log("  ✓ Query:", sqlResult.query);
console.log("  ✓ Values:", sqlResult.values);
console.log("");

// Test 15: HTML escaping (another common use case)
console.log("Test 15: HTML builder");
function html(strings, ...values) {
  let result = "";
  for (let i = 0; i < strings.length; i++) {
    result = result + strings[i];
    if (i < values.length) {
      let escaped = values[i];
      result = result + escaped;
    }
  }
  return result;
}

let title = "My Page";
let content = "Hello World";
let htmlResult = html`<html><head><title>${title}</title></head><body>${content}</body></html>`;
console.log("  ✓ HTML:", htmlResult);
console.log("");

// Test 16: Using closure instead of this
console.log("Test 16: Tag function with closure");
function makeTag(prefix) {
  return function(strings, ...values) {
    return prefix + values[0];
  };
}

let prefixTag = makeTag(">>>");
let prefixed = prefixTag`Value: ${123}`;
console.log("  ✓ Result:", prefixed);
console.log("");

// Test 17: Empty template
console.log("Test 17: Empty template");
function empty(strings) {
  return strings[0];
}

let emptyResult = empty``;
console.log("  ✓ Result:", emptyResult);
console.log("");

// Test 18: Only substitutions, no strings
console.log("Test 18: Template starting and ending with substitution");
function allSubs(strings, ...values) {
  console.log("  ✓ strings.length:", strings.length);
  console.log("  ✓ First string is empty:", strings[0] == "");
  console.log("  ✓ Last string is empty:", strings[strings.length - 1] == "");
  return values.length;
}

let count = allSubs`${1}${2}${3}`;
console.log("  ✓ Count:", count);
console.log("");

console.log("=== All 18 tests completed successfully! ===");
