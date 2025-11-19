// Test String.replace() and String.template() functionality

Ant.println("=== String.replace() and String.template() Tests ===\n");

// Test 1: Basic replace
Ant.println("Test 1: Basic replace");
let str1 = "hello world";
Ant.println("  Original:", str1);
Ant.println("  replace('world', 'there'):", str1.replace("world", "there"));
Ant.println("  replace('hello', 'goodbye'):", str1.replace("hello", "goodbye"));
Ant.println("  Original unchanged:", str1);

// Test 2: Replace only first occurrence
Ant.println("\nTest 2: Replace first occurrence only");
let str2 = "the cat and the dog";
Ant.println("  Original:", str2);
Ant.println("  replace('the', 'a'):", str2.replace("the", "a"));
let str3 = "hello hello hello";
Ant.println("  '" + str3 + "'.replace('hello', 'hi'):", str3.replace("hello", "hi"));

// Test 3: Replace not found
Ant.println("\nTest 3: Replace when not found");
let str4 = "JavaScript";
Ant.println("  '" + str4 + "'.replace('Python', 'Ruby'):", str4.replace("Python", "Ruby"));
Ant.println("  '" + str4 + "'.replace('java', 'JAVA'):", str4.replace("java", "JAVA"));

// Test 4: Replace with empty string
Ant.println("\nTest 4: Replace with empty string");
let str5 = "remove this word";
Ant.println("  '" + str5 + "'.replace('this ', ''):", str5.replace("this ", ""));
let str6 = "prefixValue";
Ant.println("  '" + str6 + "'.replace('prefix', ''):", str6.replace("prefix", ""));

// Test 5: Replace single character
Ant.println("\nTest 5: Replace single character");
let str7 = "hello";
Ant.println("  '" + str7 + "'.replace('l', 'L'):", str7.replace("l", "L"));
Ant.println("  '" + str7 + "'.replace('h', 'H'):", str7.replace("h", "H"));
Ant.println("  '" + str7 + "'.replace('o', 'O'):", str7.replace("o", "O"));

// Test 6: Replace in paths and URLs
Ant.println("\nTest 6: Replace in paths/URLs");
let path = "/api/v1/users";
Ant.println("  Path:", path);
Ant.println("  replace('/api/', '/api/v2/'):", path.replace("/api/", "/api/v2/"));
let url = "http://example.com";
Ant.println("  URL:", url);
Ant.println("  replace('http://', 'https://'):", url.replace("http://", "https://"));

// Test 7: Basic template
Ant.println("\nTest 7: Basic template()");
let tpl1 = "Hello, {{name}}!";
let data1 = { name: "Alice" };
Ant.println("  Template:", tpl1);
Ant.println("  Data: {name: 'Alice'}");
Ant.println("  Result:", tpl1.template(data1));

// Test 8: Multiple placeholders
Ant.println("\nTest 8: Multiple placeholders");
let tpl2 = "User {{name}} is {{age}} years old and lives in {{city}}";
let data2 = { name: "Bob", age: 30, city: "NYC" };
Ant.println("  Template:", tpl2);
Ant.println("  Result:", tpl2.template(data2));

// Test 9: Number and boolean values
Ant.println("\nTest 9: Different value types");
let tpl3 = "Status: {{active}}, Count: {{count}}, Rate: {{rate}}";
let data3 = { active: true, count: 42, rate: 3.14 };
Ant.println("  Template:", tpl3);
Ant.println("  Result:", tpl3.template(data3));

// Test 10: Missing keys
Ant.println("\nTest 10: Missing keys in data");
let tpl4 = "Hello {{first}} {{middle}} {{last}}";
let data4 = { first: "John", last: "Doe" };
Ant.println("  Template:", tpl4);
Ant.println("  Data: {first: 'John', last: 'Doe'}");
Ant.println("  Result:", tpl4.template(data4));
Ant.println("  (missing {{middle}} becomes empty)");

// Test 11: URL building
Ant.println("\nTest 11: URL building");
let urlTpl = "https://api.example.com/{{version}}/{{resource}}/{{id}}";
let urlData = { version: "v2", resource: "users", id: 123 };
Ant.println("  Template:", urlTpl);
Ant.println("  Result:", urlTpl.template(urlData));

// Test 12: Configuration
Ant.println("\nTest 12: Configuration messages");
let configTpl = "Server running on {{host}}:{{port}} in {{mode}} mode";
let configData = { host: "localhost", port: 3000, mode: "development" };
Ant.println("  Template:", configTpl);
Ant.println("  Result:", configTpl.template(configData));

// Test 13: Query template
Ant.println("\nTest 13: Query template");
let queryTpl = "SELECT * FROM {{table}} WHERE {{field}} = {{value}}";
let queryData = { table: "users", field: "id", value: 42 };
Ant.println("  Template:", queryTpl);
Ant.println("  Result:", queryTpl.template(queryData));

// Test 14: No placeholders
Ant.println("\nTest 14: Template with no placeholders");
let tpl6 = "This is just plain text";
let data6 = { name: "unused" };
Ant.println("  Template:", tpl6);
Ant.println("  Result:", tpl6.template(data6));

// Test 15: All placeholder
Ant.println("\nTest 15: Template is all placeholder");
let tpl7 = "{{value}}";
let data7 = { value: "replaced" };
Ant.println("  Template: '{{value}}'");
Ant.println("  Result:", tpl7.template(data7));

// Test 16: Combining replace and template
Ant.println("\nTest 16: Combining replace() and template()");
let message = "Hello {{USER}}, welcome!";
Ant.println("  Original:", message);
let normalized = message.replace("{{USER}}", "{{user}}");
Ant.println("  After replace:", normalized);
let final = normalized.template({ user: "Charlie" });
Ant.println("  After template:", final);

// Test 17: Replace after template
Ant.println("\nTest 17: Replace after template");
let greetTpl = "Hello, {{name}}!";
let greetData = { name: "World" };
let greeting = greetTpl.template(greetData);
Ant.println("  After template:", greeting);
let changed = greeting.replace("World", "Universe");
Ant.println("  After replace:", changed);

// Test 18: Validation messages
Ant.println("\nTest 18: Validation messages");
let errorTpl = "Field '{{field}}' must be at least {{min}} characters";
let errorData = { field: "password", min: 8 };
Ant.println("  Error template:", errorTpl);
Ant.println("  Result:", errorTpl.template(errorData));

let successTpl = "{{count}} items processed successfully";
let successData = { count: 150 };
Ant.println("  Success template:", successTpl);
Ant.println("  Result:", successTpl.template(successData));

// Test 19: Adjacent placeholders
Ant.println("\nTest 19: Adjacent placeholders");
let adjacent = "{{first}}{{second}}{{third}}";
let adjData = { first: "A", second: "B", third: "C" };
Ant.println("  Template:", adjacent);
Ant.println("  Result:", adjacent.template(adjData));

// Test 20: Whitespace in placeholder names
Ant.println("\nTest 20: Whitespace in placeholder names");
let wsTpl = "Hello {{userName}} from {{homeCity}}";
let wsData = { userName: "Eve", homeCity: "Boston" };
Ant.println("  Template:", wsTpl);
Ant.println("  Result:", wsTpl.template(wsData));

// Test 21: Edge cases - empty strings
Ant.println("\nTest 21: Edge cases - empty strings");
let empty = "";
Ant.println("  Empty string replace('x', 'y'):", "'" + empty.replace("x", "y") + "'");
Ant.println("  Empty string template({}):", "'" + empty.template({}) + "'");

Ant.println("\n=== All tests completed ===");
