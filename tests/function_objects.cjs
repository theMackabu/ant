// Demonstrate that functions are objects and have __code property

// Basic function
function greet(name) {
    return "Hello, " + name + "!";
}

// Functions are objects - we can add properties to them
greet.callCount = 0;
greet.description = "A greeting function";

// Accessing function properties
Ant.println("Function name: " + greet.name);
Ant.println("Description: " + greet.description);

// The __code property contains the function's bytecode/implementation
Ant.println("\n--- Function __code property ---");
Ant.println("greet.__code: " + greet.__code);
Ant.println("Type of __code: " + typeof greet.__code);

// We can call the function normally
let result = greet("World");
Ant.println("\nCalling greet('World'): " + result);

// Demonstrate with a more complex function
function fibonacci(n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

fibonacci.purpose = "Calculate Fibonacci numbers";
fibonacci.complexity = "O(2^n) - exponential";

Ant.println("\n--- Another function example ---");
Ant.println("Function: " + fibonacci.name);
Ant.println("Purpose: " + fibonacci.purpose);
Ant.println("Complexity: " + fibonacci.complexity);
Ant.println("fibonacci.__code: " + fibonacci.__code);

// Call it
Ant.println("fibonacci(7) = " + fibonacci(7));

// Functions can be stored in objects
let mathOps = {
    add: function(a, b) { return a + b; },
    multiply: function(a, b) { return a * b; }
};

Ant.println("\n--- Functions as object properties ---");
Ant.println("mathOps.add: " + mathOps.add);
Ant.println("mathOps.add.__code: " + mathOps.add.__code);
Ant.println("mathOps.add(5, 3) = " + mathOps.add(5, 3));

// Functions can be assigned to variables
let myFunc = greet;
myFunc.newProperty = "Added to the reference";

Ant.println("\n--- Function assignment ---");
Ant.println("myFunc === greet: " + (myFunc === greet));
Ant.println("myFunc.__code: " + myFunc.__code);
Ant.println("myFunc('Alice'): " + myFunc('Alice'));

// Anonymous function
let anon = function(x) { return x * 2; };
Ant.println("\n--- Anonymous function ---");
Ant.println("anon.__code: " + anon.__code);
Ant.println("anon(21) = " + anon(21));
