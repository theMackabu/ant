// Comprehensive Tagged Template Literal Tests
Ant.println("=== Tagged Template Literal Tests ===\n");

// Test 1: Basic tagged template with one substitution
Ant.println("Test 1: Basic tagged template");
function tag1(strings, ...values) {
  Ant.println("  ✓ strings.length:", strings.length);
  Ant.println("  ✓ strings[0]:", strings[0]);
  Ant.println("  ✓ strings[1]:", strings[1]);
  Ant.println("  ✓ values.length:", values.length);
  Ant.println("  ✓ values[0]:", values[0]);
  return "result1";
}

let name = "World";
let result1 = tag1`Hello ${name}!`;
Ant.println("  ✓ Result:", result1);
Ant.println("");

// Test 2: Building a string from parts
Ant.println("Test 2: Building a string from tagged template");
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
Ant.println("  ✓ Result:", greeting);
Ant.println("");

// Test 3: Multiple substitutions
Ant.println("Test 3: Multiple substitutions");
function multi(strings, ...values) {
  Ant.println("  ✓ values.length:", values.length);
  Ant.println("  ✓ First value:", values[0]);
  Ant.println("  ✓ Second value:", values[1]);
  return values[0] + values[1];
}

let a = 10;
let b = 20;
let sum = multi`Adding ${a} and ${b}`;
Ant.println("  ✓ Sum:", sum);
Ant.println("");

// Test 4: No substitutions
Ant.println("Test 4: No substitutions");
function noSub(strings) {
  Ant.println("  ✓ strings.length:", strings.length);
  Ant.println("  ✓ strings[0]:", strings[0]);
  return "no-substitutions";
}

let result4 = noSub`Just a plain string`;
Ant.println("  ✓ Result:", result4);
Ant.println("");

// Test 5: Expression in substitution
Ant.println("Test 5: Expression in substitution");
function expr(strings, ...values) {
  return values[0] * 2;
}

let x = 5;
let doubled = expr`Double ${x + 3}`;
Ant.println("  ✓ Result:", doubled);
Ant.println("");

// Test 6: Conditional in substitution
Ant.println("Test 6: Conditional in substitution");
function cond(strings, ...values) {
  return values[0];
}

let score = 85;
let grade = cond`Grade: ${score > 80 ? "A" : "B"}`;
Ant.println("  ✓ Result:", grade);
Ant.println("");

// Test 7: Object property access
Ant.println("Test 7: Object property access");
function objAccess(strings, ...values) {
  return values[0];
}

let person = { name: "Alice", age: 25 };
let info = objAccess`Name: ${person.name}`;
Ant.println("  ✓ Result:", info);
Ant.println("");

// Test 8: Array indexing
Ant.println("Test 8: Array indexing");
function arrAccess(strings, ...values) {
  return values[0];
}

let colors = ["red", "green", "blue"];
let color = arrAccess`First color: ${colors[0]}`;
Ant.println("  ✓ Result:", color);
Ant.println("");

// Test 9: Returning different types
Ant.println("Test 9: Returning different types");
function retNum(strings) {
  return 42;
}

function retBool(strings) {
  return true;
}

let num = retNum`test`;
let bool = retBool`test`;
Ant.println("  ✓ Number:", num);
Ant.println("  ✓ Boolean:", bool);
Ant.println("");

// Test 10: Using result with string methods
Ant.println("Test 10: Using result with string methods");
function makeString(strings, ...values) {
  return strings[0] + values[0] + strings[1];
}

let text = makeString`Hello ${name}!`;
Ant.println("  ✓ Text:", text);
Ant.println("  ✓ Includes 'World':", text.includes("World"));
Ant.println("  ✓ Starts with 'Hello':", text.startsWith("Hello"));
Ant.println("");

// Test 11: Rest params with many values
Ant.println("Test 11: Rest params with many values");
function manyValues(strings, ...values) {
  Ant.println("  ✓ strings.length:", strings.length);
  Ant.println("  ✓ values.length:", values.length);
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
Ant.println("  ✓ Result:", assembled);
Ant.println("");

// Test 12: Nested function calls
Ant.println("Test 12: Nested expressions");
function nested(strings, ...values) {
  return values[0] + values[1];
}

function double(n) {
  return n * 2;
}

let nested_result = nested`${double(5)} + ${double(10)}`;
Ant.println("  ✓ Result:", nested_result);
Ant.println("");

// Test 13: Empty strings between substitutions
Ant.println("Test 13: Adjacent substitutions");
function adjacent(strings, ...values) {
  Ant.println("  ✓ strings:", strings);
  Ant.println("  ✓ values:", values);
  return values[0] + values[1];
}

let adj = adjacent`${10}${20}`;
Ant.println("  ✓ Result:", adj);
Ant.println("");

// Test 14: SQL-style template (common use case)
Ant.println("Test 14: SQL-style builder");
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
Ant.println("  ✓ Query:", sqlResult.query);
Ant.println("  ✓ Values:", sqlResult.values);
Ant.println("");

// Test 15: HTML escaping (another common use case)
Ant.println("Test 15: HTML builder");
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
Ant.println("  ✓ HTML:", htmlResult);
Ant.println("");

// Test 16: Using closure instead of this
Ant.println("Test 16: Tag function with closure");
function makeTag(prefix) {
  return function(strings, ...values) {
    return prefix + values[0];
  };
}

let prefixTag = makeTag(">>>");
let prefixed = prefixTag`Value: ${123}`;
Ant.println("  ✓ Result:", prefixed);
Ant.println("");

// Test 17: Empty template
Ant.println("Test 17: Empty template");
function empty(strings) {
  return strings[0];
}

let emptyResult = empty``;
Ant.println("  ✓ Result:", emptyResult);
Ant.println("");

// Test 18: Only substitutions, no strings
Ant.println("Test 18: Template starting and ending with substitution");
function allSubs(strings, ...values) {
  Ant.println("  ✓ strings.length:", strings.length);
  Ant.println("  ✓ First string is empty:", strings[0] == "");
  Ant.println("  ✓ Last string is empty:", strings[strings.length - 1] == "");
  return values.length;
}

let count = allSubs`${1}${2}${3}`;
Ant.println("  ✓ Count:", count);
Ant.println("");

Ant.println("=== All 18 tests completed successfully! ===");
