// Test default parameters in function definitions

// Test 1: Basic default parameter
function greet(name = 'World') {
    return 'Hello, ' + name;
}

console.log("Test 1 - Basic default param:");
console.log(greet()); // Should print "Hello, World"
console.log(greet('Alice')); // Should print "Hello, Alice"

// Test 2: Multiple default parameters
function multiply(a = 1, b = 1, c = 1) {
    return a * b * c;
}

console.log("\nTest 2 - Multiple default params:");
console.log(multiply()); // Should print 1
console.log(multiply(2)); // Should print 2
console.log(multiply(2, 3)); // Should print 6
console.log(multiply(2, 3, 4)); // Should print 24

// Test 3: Mix of regular and default parameters
function describe(name, age = 25, city = 'Unknown') {
    return name + ' is ' + age + ' years old and lives in ' + city;
}

console.log("\nTest 3 - Mix of regular and default params:");
console.log(describe('Alice')); // Should print "Alice is 25 years old and lives in Unknown"
console.log(describe('Bob', 30)); // Should print "Bob is 30 years old and lives in Unknown"
console.log(describe('Charlie', 35, 'NYC')); // Should print "Charlie is 35 years old and lives in NYC"

// Test 4: Default parameter with number
function power(base = 2, exponent = 2) {
    let result = 1;
    for (let i = 0; i < exponent; i++) {
        result = result * base;
    }
    return result;
}

console.log("\nTest 4 - Default params with numbers:");
console.log(power()); // Should print 4 (2^2)
console.log(power(3)); // Should print 9 (3^2)
console.log(power(2, 3)); // Should print 8 (2^3)

// Test 5: Arrow function with default parameters
const add = (a = 0, b = 0) => a + b;

console.log("\nTest 5 - Arrow function with default params:");
console.log(add()); // Should print 0
console.log(add(5)); // Should print 5
console.log(add(5, 3)); // Should print 8

// Test 6: Class methods with default parameters
class Calculator {
    add(x = 0, y = 0) {
        return x + y;
    }
    
    subtract(x = 0, y = 0) {
        return x - y;
    }
    
    multiply(x = 1, y = 1) {
        return x * y;
    }
}

console.log("\nTest 6 - Class methods with default params:");
const calc = new Calculator();
console.log(calc.add()); // Should print 0
console.log(calc.add(10)); // Should print 10
console.log(calc.add(10, 5)); // Should print 15
console.log(calc.multiply()); // Should print 1
console.log(calc.multiply(3)); // Should print 3
console.log(calc.multiply(3, 4)); // Should print 12

// Test 7: Default params with special characters in strings
function wrap(text, left = '(', right = ')') {
    return left + text + right;
}

console.log("\nTest 7 - Default params with parens in strings:");
console.log(wrap('hello')); // Should print "(hello)"
console.log(wrap('hello', '<', '>')); // Should print "<hello>"
console.log(wrap('hello', '[')); // Should print "[hello)"

// Test 8: Default with undefined explicitly passed
function testUndefined(a = 'default') {
    return a;
}

console.log("\nTest 8 - Explicit undefined uses default:");
console.log(testUndefined()); // Should print "default"
console.log(testUndefined(undefined)); // Should print "default"
console.log(testUndefined('value')); // Should print "value"

// Test 9: Complex default expressions
function createArray(size = 5, fill = 'x') {
    let arr = [];
    for (let i = 0; i < size; i++) {
        arr.push(fill);
    }
    return arr;
}

console.log("\nTest 9 - Default params with expressions:");
console.log(createArray().length); // Should print 5
console.log(createArray(3).length); // Should print 3
console.log(createArray(2, 'o').join('')); // Should print "oo"

// Test 10: Async function with default parameters
async function asyncGreet(name = 'Async World') {
    return 'Hello, ' + name;
}

console.log("\nTest 10 - Async function with default params:");
asyncGreet().then(result => console.log(result)); // Should print "Hello, Async World"
asyncGreet('Async Alice').then(result => console.log(result)); // Should print "Hello, Async Alice"

// Test 11: Default parameters in constructor
class Person {
    constructor(name = 'Anonymous', age = 0) {
        this.name = name;
        this.age = age;
    }
    
    introduce() {
        return this.name + ' is ' + this.age;
    }
}

console.log("\nTest 11 - Constructor with default params:");
const p1 = new Person();
console.log(p1.introduce()); // Should print "Anonymous is 0"
const p2 = new Person('Dave');
console.log(p2.introduce()); // Should print "Dave is 0"
const p3 = new Person('Eve', 28);
console.log(p3.introduce()); // Should print "Eve is 28"

// Test 12: Only last parameters have defaults
function compute(required, optional = 10) {
    return required + optional;
}

console.log("\nTest 12 - Required param then default:");
console.log(compute(5)); // Should print 15
console.log(compute(5, 20)); // Should print 25

console.log("\n✓ All default parameter tests completed!");
