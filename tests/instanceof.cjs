// Test instanceof operator and built-in constructors

// Test instanceof with primitives
let str = "hello";
let num = 42;
let bool = true;
let obj = {};

console.log(str instanceof String);      // true
console.log(num instanceof Number);      // true
console.log(bool instanceof Boolean);    // true
console.log(obj instanceof Object);      // true

// Test instanceof with wrong types
console.log(str instanceof Number);      // false
console.log(num instanceof String);      // false
console.log(bool instanceof Object);     // false
console.log(obj instanceof Function);    // false

// Test instanceof with functions
function myFunc() {
  return 42;
}

let funcExpr = function() {
  return "hello";
};

console.log(myFunc instanceof Function); // true
console.log(funcExpr instanceof Function); // true
console.log(myFunc instanceof Object);   // false

// Test String() constructor
console.log(String(42));                 // "42"
console.log(String(true));               // "true"
console.log(String(false));              // "false"
console.log(String(null));               // "null"
console.log(String({}));                 // [object Object] or similar

let converted = String(123);
console.log(converted instanceof String); // true
console.log(typeof converted);           // string

// Test Number() constructor
console.log(Number("123"));              // 123
console.log(Number("3.14"));             // 3.14
console.log(Number(true));               // 1
console.log(Number(false));              // 0
console.log(Number(null));               // 0

let numConverted = Number("456");
console.log(numConverted instanceof Number); // true
console.log(typeof numConverted);        // number

// Test Boolean() constructor
console.log(Boolean(1));                 // true
console.log(Boolean(0));                 // false
console.log(Boolean("hello"));           // true
console.log(Boolean(""));                // false
console.log(Boolean(null));              // false
console.log(Boolean({}));                // true

let boolConverted = Boolean("test");
console.log(boolConverted instanceof Boolean); // true
console.log(typeof boolConverted);       // boolean

// Test Object() constructor
let emptyObj = Object();
console.log(typeof emptyObj);            // object
console.log(emptyObj instanceof Object); // true

// Compare with typeof
console.log(typeof str);                 // string
console.log(typeof num);                 // number
console.log(typeof bool);                // boolean
console.log(typeof obj);                 // object
console.log(typeof myFunc);              // function

// Mixed operations
let x = Number("10");
let y = Number("20");
let result = x + y;
console.log(result);                     // 30
console.log(result instanceof Number);   // true

let greeting = String("Hello ") + String("World");
console.log(greeting);                   // Hello World
console.log(greeting instanceof String); // true
