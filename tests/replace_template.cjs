// Test String.replace() and String.template() functionality

console.log("=== String.replace() and String.template() Tests ===\n");

// Test 1: Basic replace
console.log("Test 1: Basic replace");
let str1 = "hello world";
console.log("  Original:", str1);
console.log("  replace('world', 'there'):", str1.replace("world", "there"));
console.log("  replace('hello', 'goodbye'):", str1.replace("hello", "goodbye"));
console.log("  Original unchanged:", str1);

// Test 2: Replace only first occurrence
console.log("\nTest 2: Replace first occurrence only");
let str2 = "the cat and the dog";
console.log("  Original:", str2);
console.log("  replace('the', 'a'):", str2.replace("the", "a"));
let str3 = "hello hello hello";
console.log("  '" + str3 + "'.replace('hello', 'hi'):", str3.replace("hello", "hi"));

// Test 3: Replace not found
console.log("\nTest 3: Replace when not found");
let str4 = "JavaScript";
console.log("  '" + str4 + "'.replace('Python', 'Ruby'):", str4.replace("Python", "Ruby"));
console.log("  '" + str4 + "'.replace('java', 'JAVA'):", str4.replace("java", "JAVA"));

// Test 4: Replace with empty string
console.log("\nTest 4: Replace with empty string");
let str5 = "remove this word";
console.log("  '" + str5 + "'.replace('this ', ''):", str5.replace("this ", ""));
let str6 = "prefixValue";
console.log("  '" + str6 + "'.replace('prefix', ''):", str6.replace("prefix", ""));

// Test 5: Replace single character
console.log("\nTest 5: Replace single character");
let str7 = "hello";
console.log("  '" + str7 + "'.replace('l', 'L'):", str7.replace("l", "L"));
console.log("  '" + str7 + "'.replace('h', 'H'):", str7.replace("h", "H"));
console.log("  '" + str7 + "'.replace('o', 'O'):", str7.replace("o", "O"));

// Test 6: Replace in paths and URLs
console.log("\nTest 6: Replace in paths/URLs");
let path = "/api/v1/users";
console.log("  Path:", path);
console.log("  replace('/api/', '/api/v2/'):", path.replace("/api/", "/api/v2/"));
let url = "http://example.com";
console.log("  URL:", url);
console.log("  replace('http://', 'https://'):", url.replace("http://", "https://"));

// Test 7: Basic template
console.log("\nTest 7: Basic template()");
let tpl1 = "Hello, {{name}}!";
let data1 = { name: "Alice" };
console.log("  Template:", tpl1);
console.log("  Data: {name: 'Alice'}");
console.log("  Result:", tpl1.template(data1));

// Test 8: Multiple placeholders
console.log("\nTest 8: Multiple placeholders");
let tpl2 = "User {{name}} is {{age}} years old and lives in {{city}}";
let data2 = { name: "Bob", age: 30, city: "NYC" };
console.log("  Template:", tpl2);
console.log("  Result:", tpl2.template(data2));

// Test 9: Number and boolean values
console.log("\nTest 9: Different value types");
let tpl3 = "Status: {{active}}, Count: {{count}}, Rate: {{rate}}";
let data3 = { active: true, count: 42, rate: 3.14 };
console.log("  Template:", tpl3);
console.log("  Result:", tpl3.template(data3));

// Test 10: Missing keys
console.log("\nTest 10: Missing keys in data");
let tpl4 = "Hello {{first}} {{middle}} {{last}}";
let data4 = { first: "John", last: "Doe" };
console.log("  Template:", tpl4);
console.log("  Data: {first: 'John', last: 'Doe'}");
console.log("  Result:", tpl4.template(data4));
console.log("  (missing {{middle}} becomes empty)");

// Test 11: URL building
console.log("\nTest 11: URL building");
let urlTpl = "https://api.example.com/{{version}}/{{resource}}/{{id}}";
let urlData = { version: "v2", resource: "users", id: 123 };
console.log("  Template:", urlTpl);
console.log("  Result:", urlTpl.template(urlData));

// Test 12: Configuration
console.log("\nTest 12: Configuration messages");
let configTpl = "Server running on {{host}}:{{port}} in {{mode}} mode";
let configData = { host: "localhost", port: 3000, mode: "development" };
console.log("  Template:", configTpl);
console.log("  Result:", configTpl.template(configData));

// Test 13: Query template
console.log("\nTest 13: Query template");
let queryTpl = "SELECT * FROM {{table}} WHERE {{field}} = {{value}}";
let queryData = { table: "users", field: "id", value: 42 };
console.log("  Template:", queryTpl);
console.log("  Result:", queryTpl.template(queryData));

// Test 14: No placeholders
console.log("\nTest 14: Template with no placeholders");
let tpl6 = "This is just plain text";
let data6 = { name: "unused" };
console.log("  Template:", tpl6);
console.log("  Result:", tpl6.template(data6));

// Test 15: All placeholder
console.log("\nTest 15: Template is all placeholder");
let tpl7 = "{{value}}";
let data7 = { value: "replaced" };
console.log("  Template: '{{value}}'");
console.log("  Result:", tpl7.template(data7));

// Test 16: Combining replace and template
console.log("\nTest 16: Combining replace() and template()");
let message = "Hello {{USER}}, welcome!";
console.log("  Original:", message);
let normalized = message.replace("{{USER}}", "{{user}}");
console.log("  After replace:", normalized);
let final = normalized.template({ user: "Charlie" });
console.log("  After template:", final);

// Test 17: Replace after template
console.log("\nTest 17: Replace after template");
let greetTpl = "Hello, {{name}}!";
let greetData = { name: "World" };
let greeting = greetTpl.template(greetData);
console.log("  After template:", greeting);
let changed = greeting.replace("World", "Universe");
console.log("  After replace:", changed);

// Test 18: Validation messages
console.log("\nTest 18: Validation messages");
let errorTpl = "Field '{{field}}' must be at least {{min}} characters";
let errorData = { field: "password", min: 8 };
console.log("  Error template:", errorTpl);
console.log("  Result:", errorTpl.template(errorData));

let successTpl = "{{count}} items processed successfully";
let successData = { count: 150 };
console.log("  Success template:", successTpl);
console.log("  Result:", successTpl.template(successData));

// Test 19: Adjacent placeholders
console.log("\nTest 19: Adjacent placeholders");
let adjacent = "{{first}}{{second}}{{third}}";
let adjData = { first: "A", second: "B", third: "C" };
console.log("  Template:", adjacent);
console.log("  Result:", adjacent.template(adjData));

// Test 20: Whitespace in placeholder names
console.log("\nTest 20: Whitespace in placeholder names");
let wsTpl = "Hello {{userName}} from {{homeCity}}";
let wsData = { userName: "Eve", homeCity: "Boston" };
console.log("  Template:", wsTpl);
console.log("  Result:", wsTpl.template(wsData));

// Test 21: Edge cases - empty strings
console.log("\nTest 21: Edge cases - empty strings");
let empty = "";
console.log("  Empty string replace('x', 'y'):", "'" + empty.replace("x", "y") + "'");
console.log("  Empty string template({}):", "'" + empty.template({}) + "'");

console.log("\n=== All tests completed ===");
