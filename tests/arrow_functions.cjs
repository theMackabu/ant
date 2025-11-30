// Test 1: Basic arrow function with two parameters and expression body
let add = (a, b) => a + b;
let result1 = add(5, 3);
Ant.println("Test 1 - add(5, 3):", result1); // Should be 8

// Test 2: Arrow function with single parameter (no parentheses)
let double = x => x * 2;
let result2 = double(7);
Ant.println("Test 2 - double(7):", result2); // Should be 14

// Test 3: Arrow function with block body
let multiply = (a, b) => {
  let result = a * b;
  return result;
};
let result3 = multiply(4, 6);
Ant.println("Test 3 - multiply(4, 6):", result3); // Should be 24

// Test 4: Arrow function with no parameters
let getFortyTwo = () => 42;
let result4 = getFortyTwo();
Ant.println("Test 4 - getFortyTwo():", result4); // Should be 42

// Test 5: Arrow function with single expression
let square = x => x * x;
let result5 = square(5);
Ant.println("Test 5 - square(5):", result5); // Should be 25

// Test 6: Nested arrow functions (currying)
let makeAdder = x => y => x + y;
let add5 = makeAdder(5);
let result6 = add5(3);
Ant.println("Test 6 - makeAdder(5)(3):", result6); // Should be 8

// Test 7: Arrow function with multiple statements in block
let complexCalc = (a, b) => {
  let sum = a + b;
  let product = a * b;
  let result = sum + product;
  return result;
};
let result7 = complexCalc(3, 4);
Ant.println("Test 7 - complexCalc(3, 4):", result7); // Should be 19 (7 + 12)

// Test 8: Arrow function with conditional
let max = (a, b) => {
  if (a > b) {
    return a;
  } else {
    return b;
  }
};
let result8 = max(10, 15);
Ant.println("Test 8 - max(10, 15):", result8); // Should be 15

// Test 9: Arrow function with string concatenation
let greet = name => "Hello, " + name;
let result9 = greet("World");
Ant.println("Test 9 - greet('World'):", result9); // Should be "Hello, World"

// Test 10: Arrow function with comparison
let isPositive = x => x > 0;
let result10a = isPositive(5);
Ant.println("Test 10a - isPositive(5):", result10a); // Should be true
let result10b = isPositive(-3);
Ant.println("Test 10b - isPositive(-3):", result10b); // Should be false

// Test 11: Arrow function in array
let funcs = [];
funcs[0] = x => x + 1;
funcs[1] = x => x * 2;
let result11a = funcs[0](10);
Ant.println("Test 11a - funcs[0](10):", result11a); // Should be 11
let result11b = funcs[1](10);
Ant.println("Test 11b - funcs[1](10):", result11b); // Should be 20

// Test 12: Arrow function with three parameters
let sum3 = (a, b, c) => a + b + c;
let result12 = sum3(1, 2, 3);
Ant.println("Test 12 - sum3(1, 2, 3):", result12); // Should be 6

// Test 13: Arrow function assigned to object property
let calculator = {};
calculator.add = (a, b) => a + b;
calculator.subtract = (a, b) => a - b;
let result13a = calculator.add(10, 5);
Ant.println("Test 13a - calculator.add(10, 5):", result13a); // Should be 15
let result13b = calculator.subtract(10, 5);
Ant.println("Test 13b - calculator.subtract(10, 5):", result13b); // Should be 5

// Test 14: Arrow function returning early
let checkValue = x => {
  if (x < 0) {
    return "negative";
  }
  if (x === 0) {
    return "zero";
  }
  return "positive";
};
let result14a = checkValue(-5);
Ant.println("Test 14a - checkValue(-5):", result14a); // Should be "negative"
let result14b = checkValue(0);
Ant.println("Test 14b - checkValue(0):", result14b); // Should be "zero"
let result14c = checkValue(5);
Ant.println("Test 14c - checkValue(5):", result14c); // Should be "positive"

// Test 15: Arrow function with implicit undefined return
let noReturn = x => {
  let temp = x + 1;
};
let result15 = noReturn(5);
Ant.println("Test 15 - noReturn(5):", result15); // Should be undefined

Ant.println("\nAll arrow function tests completed!");
