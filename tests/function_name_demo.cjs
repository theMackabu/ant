// Demonstrate function.name property

function myFunction() {
    return "Hello";
}

function anotherFunction(x, y) {
    return x + y;
}

console.log("Function names:");
console.log("myFunction.name = " + myFunction.name);
console.log("anotherFunction.name = " + anotherFunction.name);

// Anonymous function has empty name
let anon = function() { return 42; };
console.log("anon.name = '" + anon.name + "' (empty for anonymous functions)");

// Named function expression
let named = function namedFunc() { return 100; };
console.log("named.name = " + named.name);
