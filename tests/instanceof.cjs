// Test instanceof operator and built-in constructors

// Test instanceof with primitives
let str = "hello";
let num = 42;
let bool = true;
let obj = {};

Ant.println(str instanceof String);      // true
Ant.println(num instanceof Number);      // true
Ant.println(bool instanceof Boolean);    // true
Ant.println(obj instanceof Object);      // true

// Test instanceof with wrong types
Ant.println(str instanceof Number);      // false
Ant.println(num instanceof String);      // false
Ant.println(bool instanceof Object);     // false
Ant.println(obj instanceof Function);    // false

// Test instanceof with functions
function myFunc() {
  return 42;
}

let funcExpr = function() {
  return "hello";
};

Ant.println(myFunc instanceof Function); // true
Ant.println(funcExpr instanceof Function); // true
Ant.println(myFunc instanceof Object);   // false

// Test String() constructor
Ant.println(String(42));                 // "42"
Ant.println(String(true));               // "true"
Ant.println(String(false));              // "false"
Ant.println(String(null));               // "null"
Ant.println(String({}));                 // [object Object] or similar

let converted = String(123);
Ant.println(converted instanceof String); // true
Ant.println(typeof converted);           // string

// Test Number() constructor
Ant.println(Number("123"));              // 123
Ant.println(Number("3.14"));             // 3.14
Ant.println(Number(true));               // 1
Ant.println(Number(false));              // 0
Ant.println(Number(null));               // 0

let numConverted = Number("456");
Ant.println(numConverted instanceof Number); // true
Ant.println(typeof numConverted);        // number

// Test Boolean() constructor
Ant.println(Boolean(1));                 // true
Ant.println(Boolean(0));                 // false
Ant.println(Boolean("hello"));           // true
Ant.println(Boolean(""));                // false
Ant.println(Boolean(null));              // false
Ant.println(Boolean({}));                // true

let boolConverted = Boolean("test");
Ant.println(boolConverted instanceof Boolean); // true
Ant.println(typeof boolConverted);       // boolean

// Test Object() constructor
let emptyObj = Object();
Ant.println(typeof emptyObj);            // object
Ant.println(emptyObj instanceof Object); // true

// Compare with typeof
Ant.println(typeof str);                 // string
Ant.println(typeof num);                 // number
Ant.println(typeof bool);                // boolean
Ant.println(typeof obj);                 // object
Ant.println(typeof myFunc);              // function

// Mixed operations
let x = Number("10");
let y = Number("20");
let result = x + y;
Ant.println(result);                     // 30
Ant.println(result instanceof Number);   // true

let greeting = String("Hello ") + String("World");
Ant.println(greeting);                   // Hello World
Ant.println(greeting instanceof String); // true
