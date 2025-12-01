// Test JSON.parse and JSON.stringify functionality

console.log("=== JSON Tests ===\n");

// Test 1: Parse simple object
console.log("Test 1: Parse simple object");
let jsonStr1 = '{"name":"John","age":30}';
console.log("  Input:", jsonStr1);
let obj1 = JSON.parse(jsonStr1);
console.log("  Parsed:", obj1);
console.log("  Name:", obj1.name);
console.log("  Age:", obj1.age);

// Test 2: Parse array
console.log("\nTest 2: Parse array");
let jsonStr2 = '[1,2,3,4,5]';
console.log("  Input:", jsonStr2);
let arr2 = JSON.parse(jsonStr2);
console.log("  Parsed:", arr2);
console.log("  Length:", arr2.length);
console.log("  First:", arr2[0]);
console.log("  Last:", arr2[4]);

// Test 3: Parse nested object
console.log("\nTest 3: Parse nested object");
let jsonStr3 = '{"user":{"name":"Alice","age":25},"active":true}';
console.log("  Input:", jsonStr3);
let obj3 = JSON.parse(jsonStr3);
console.log("  Parsed:", obj3);
console.log("  User:", obj3.user);
console.log("  User name:", obj3.user.name);
console.log("  Active:", obj3.active);

// Test 4: Parse array of objects
console.log("\nTest 4: Parse array of objects");
let jsonStr4 = '[{"id":1,"name":"Item1"},{"id":2,"name":"Item2"}]';
console.log("  Input:", jsonStr4);
let arr4 = JSON.parse(jsonStr4);
console.log("  Parsed:", arr4);
console.log("  Length:", arr4.length);
console.log("  First item:", arr4[0]);
console.log("  First item name:", arr4[0].name);
console.log("  Second item:", arr4[1]);
console.log("  Second item id:", arr4[1].id);

// Test 5: Parse with null and boolean values
console.log("\nTest 5: Parse with null and boolean values");
let jsonStr5 = '{"name":"Bob","active":true,"inactive":false,"data":null}';
console.log("  Input:", jsonStr5);
let obj5 = JSON.parse(jsonStr5);
console.log("  Parsed:", obj5);
console.log("  Name:", obj5.name);
console.log("  Active:", obj5.active);
console.log("  Inactive:", obj5.inactive);
console.log("  Data:", obj5.data);

// Test 6: Parse numbers
console.log("\nTest 6: Parse numbers");
let jsonStr6 = '{"int":42,"float":3.14,"negative":-10,"zero":0}';
console.log("  Input:", jsonStr6);
let obj6 = JSON.parse(jsonStr6);
console.log("  Parsed:", obj6);
console.log("  Integer:", obj6.int);
console.log("  Float:", obj6.float);
console.log("  Negative:", obj6.negative);
console.log("  Zero:", obj6.zero);

// Test 7: Parse empty structures
console.log("\nTest 7: Parse empty structures");
let emptyObj = JSON.parse('{}');
let emptyArr = JSON.parse('[]');
console.log("  Empty object:", emptyObj);
console.log("  Empty array:", emptyArr);
console.log("  Empty array length:", emptyArr.length);

// Test 8: Stringify array (parsed from JSON)
console.log("\nTest 8: Stringify array");
let arrStr = '[10,20,30,40]';
console.log("  Input JSON:", arrStr);
let arr8 = JSON.parse(arrStr);
console.log("  Parsed array:", arr8);
let stringified2 = JSON.stringify(arr8);
console.log("  Stringified:", stringified2);

// Test 9: Stringify object (parsed from JSON)
console.log("\nTest 9: Stringify object");
let objStr9 = '{"name":"Charlie","age":35}';
console.log("  Input JSON:", objStr9);
let obj9 = JSON.parse(objStr9);
console.log("  Parsed object:", obj9);
let stringified9 = JSON.stringify(obj9);
console.log("  Stringified:", stringified9);

// Test 10: Stringify nested object (parsed from JSON)
console.log("\nTest 10: Stringify nested object");
let nestedStr = '{"user":{"name":"David","email":"david@example.com"},"count":42}';
console.log("  Input JSON:", nestedStr);
let nested10 = JSON.parse(nestedStr);
console.log("  Parsed:", nested10);
let stringified10 = JSON.stringify(nested10);
console.log("  Stringified:", stringified10);

// Test 11: Stringify with various types
console.log("\nTest 11: Stringify with various types");
let typesStr = '{"active":true,"inactive":false,"data":null,"count":123}';
console.log("  Input JSON:", typesStr);
let types11 = JSON.parse(typesStr);
console.log("  Parsed:", types11);
let stringified11 = JSON.stringify(types11);
console.log("  Stringified:", stringified11);

// Test 12: Round-trip test (parse -> modify)
console.log("\nTest 12: Round-trip test");
let original = '{"count":5,"items":["a","b","c"]}';
console.log("  Original:", original);
let parsed = JSON.parse(original);
console.log("  Parsed:", parsed);
parsed.count = 10;
console.log("  Modified count:", parsed.count);
console.log("  Items:", parsed.items);
console.log("  Items length:", parsed.items.length);

// Test 13: Parse and access properties
console.log("\nTest 13: Parse and access properties");
let configStr = '{"host":"localhost","port":8080,"ssl":true}';
console.log("  Config:", configStr);
let config = JSON.parse(configStr);
console.log("  Host:", config.host);
console.log("  Port:", config.port);
console.log("  SSL:", config.ssl);

// Test 14: Array operations after parse
console.log("\nTest 14: Array operations after parse");
let numbersStr = '[5,10,15,20,25]';
console.log("  Numbers:", numbersStr);
let numbers = JSON.parse(numbersStr);
console.log("  Parsed:", numbers);
console.log("  Length:", numbers.length);

let sum = 0;
for (let i = 0; i < numbers.length; i = i + 1) {
  sum = sum + numbers[i];
}
console.log("  Sum:", sum);

// Test 15: Parse and re-stringify array
console.log("\nTest 15: Parse and re-stringify array");
let arrayStr15 = '[1,2,3,4,5]';
console.log("  Original:", arrayStr15);
let myArray = JSON.parse(arrayStr15);
console.log("  Parsed:", myArray);
console.log("  Length:", myArray.length);
let reStringified = JSON.stringify(myArray);
console.log("  Re-stringified:", reStringified);

// Test 16: Parse string values
console.log("\nTest 16: Parse string values");
let stringsObj = '{"greeting":"Hello","message":"World"}';
console.log("  Input:", stringsObj);
let strings = JSON.parse(stringsObj);
console.log("  Greeting:", strings.greeting);
console.log("  Message:", strings.message);
console.log("  Combined:", strings.greeting + " " + strings.message);

// Test 17: Complex nested structure
console.log("\nTest 17: Complex nested structure");
let complexStr = '{"users":[{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}],"total":2}';
console.log("  Input:", complexStr);
let complex = JSON.parse(complexStr);
console.log("  Total users:", complex.total);
console.log("  First user:", complex.users[0].name);
console.log("  Second user:", complex.users[1].name);

// Test 18: Parse, modify, and re-stringify
console.log("\nTest 18: Parse, modify, and re-stringify");
let modStr = '{"id":123,"name":"Test"}';
console.log("  Original:", modStr);
let newObj = JSON.parse(modStr);
console.log("  Parsed:", newObj);
newObj.active = true;
console.log("  After modification:", newObj);
console.log("  Active property:", newObj.active);

// Test 19: Parse whitespace-heavy JSON
console.log("\nTest 19: Parse JSON with whitespace");
let spacedJson = '  {  "key"  :  "value"  }  ';
console.log("  Input:", spacedJson);
let spacedParsed = JSON.parse(spacedJson);
console.log("  Parsed:", spacedParsed);
console.log("  Key:", spacedParsed.key);

// Test 20: Practical example - API response
console.log("\nTest 20: Practical example - API response");
let apiResponse = '{"status":"success","data":{"user":"john","token":"abc123"},"timestamp":1234567890}';
console.log("  API Response:", apiResponse);
let response = JSON.parse(apiResponse);
console.log("  Status:", response.status);
console.log("  User:", response.data.user);
console.log("  Token:", response.data.token);
console.log("  Timestamp:", response.timestamp);

console.log("\n=== All JSON tests completed ===");
