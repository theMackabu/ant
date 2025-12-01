// Demonstrate that functions are objects and have __code property

// Basic function
function greet(name) {
    return "Hello, " + name + "!";
}

// Functions are objects - we can add properties to them
greet.callCount = 0;
greet.description = "A greeting function";

// Accessing function properties
console.log("Function name: " + greet.name);
console.log("Description: " + greet.description);

// The __code property contains the function's bytecode/implementation
console.log("\n--- Function __code property ---");
console.log("greet.__code: " + greet.__code);
console.log("Type of __code: " + typeof greet.__code);

// We can call the function normally
let result = greet("World");
console.log("\nCalling greet('World'): " + result);

// Demonstrate with a more complex function
function fibonacci(n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

fibonacci.purpose = "Calculate Fibonacci numbers";
fibonacci.complexity = "O(2^n) - exponential";

console.log("\n--- Another function example ---");
console.log("Function: " + fibonacci.name);
console.log("Purpose: " + fibonacci.purpose);
console.log("Complexity: " + fibonacci.complexity);
console.log("fibonacci.__code: " + fibonacci.__code);

// Call it
console.log("fibonacci(7) = " + fibonacci(7));

// Functions can be stored in objects
let mathOps = {
    add: function(a, b) { return a + b; },
    multiply: function(a, b) { return a * b; }
};

console.log("\n--- Functions as object properties ---");
console.log("mathOps.add: " + mathOps.add);
console.log("mathOps.add.__code: " + mathOps.add.__code);
console.log("mathOps.add(5, 3) = " + mathOps.add(5, 3));

// Functions can be assigned to variables
let myFunc = greet;
myFunc.newProperty = "Added to the reference";

console.log("\n--- Function assignment ---");
console.log("myFunc === greet: " + (myFunc === greet));
console.log("myFunc.__code: " + myFunc.__code);
console.log("myFunc('Alice'): " + myFunc('Alice'));

// Anonymous function
let anon = function(x) { return x * 2; };
console.log("\n--- Anonymous function ---");
console.log("anon.__code: " + anon.__code);
console.log("anon(21) = " + anon(21));
