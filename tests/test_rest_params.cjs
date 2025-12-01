// Test rest parameters in function definitions

// Test 1: Basic rest parameter
function sum(...numbers) {
    let total = 0;
    for (let i = 0; i < numbers.length; i++) {
        total = total + numbers[i];
    }
    return total;
}

console.log("Test 1 - Basic rest params:");
console.log(sum(1, 2, 3, 4, 5)); // Should print 15

// Test 2: Rest parameter with regular parameters
function greet(greeting, ...names) {
    let result = greeting;
    for (let i = 0; i < names.length; i++) {
        result = result + " " + names[i];
    }
    return result;
}

console.log("\nTest 2 - Rest params with regular params:");
console.log(greet("Hello", "Alice", "Bob", "Charlie")); // Should print "Hello Alice Bob Charlie"

// Test 3: Rest parameter with no arguments passed
function noArgs(...args) {
    return args.length;
}

console.log("\nTest 3 - Rest params with no args:");
console.log(noArgs()); // Should print 0

// Test 4: Rest parameter with single argument
function singleArg(...args) {
    return args[0];
}

console.log("\nTest 4 - Rest params with single arg:");
console.log(singleArg(42)); // Should print 42

// Test 5: Arrow function with rest parameters
const multiply = (...factors) => {
    let result = 1;
    for (let i = 0; i < factors.length; i++) {
        result = result * factors[i];
    }
    return result;
};

console.log("\nTest 5 - Arrow function with rest params:");
console.log(multiply(2, 3, 4)); // Should print 24

// Test 6: Multiple regular params with rest
function compute(operation, initial, ...values) {
    let result = initial;
    if (operation === "add") {
        for (let i = 0; i < values.length; i++) {
            result = result + values[i];
        }
    }
    if (operation === "multiply") {
        for (let i = 0; i < values.length; i++) {
            result = result * values[i];
        }
    }
    return result;
}

console.log("\nTest 6 - Multiple params with rest:");
console.log(compute("add", 10, 5, 3, 2)); // Should print 20
console.log(compute("multiply", 2, 3, 4)); // Should print 24

// Test 7: Rest parameter is an actual array
function checkArray(...items) {
    return items.length;
}

console.log("\nTest 7 - Rest param is array:");
console.log(checkArray("a", "b", "c", "d")); // Should print 4
