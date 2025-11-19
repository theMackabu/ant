// Demonstrate function.name property

function myFunction() {
    return "Hello";
}

function anotherFunction(x, y) {
    return x + y;
}

Ant.println("Function names:");
Ant.println("myFunction.name = " + myFunction.name);
Ant.println("anotherFunction.name = " + anotherFunction.name);

// Anonymous function has empty name
let anon = function() { return 42; };
Ant.println("anon.name = '" + anon.name + "' (empty for anonymous functions)");

// Named function expression
let named = function namedFunc() { return 100; };
Ant.println("named.name = " + named.name);
