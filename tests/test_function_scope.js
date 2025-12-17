// Test that window/globalThis remain global inside functions

console.log("=== Global Scope ===");
globalThis.globalVar = "I am global";
console.log("globalThis.globalVar:", globalThis.globalVar);

console.log("\n=== Inside Regular Function ===");
function testRegularFunc() {
  console.log("Inside function, globalThis.globalVar:", globalThis.globalVar);
  console.log("Inside function, window.globalVar:", window.globalVar);
  
  // Set a new property from inside the function
  window.fromFunction = "set from inside function";
  globalThis.alsoFromFunction = "also from inside";
  
  // Test that window/globalThis are the same
  console.log("Inside function, window === globalThis:", window === globalThis);
}

testRegularFunc();
console.log("After function, window.fromFunction:", window.fromFunction);
console.log("After function, globalThis.alsoFromFunction:", globalThis.alsoFromFunction);

console.log("\n=== Inside Arrow Function ===");
const testArrowFunc = () => {
  console.log("Arrow function, globalThis.globalVar:", globalThis.globalVar);
  console.log("Arrow function, window.globalVar:", window.globalVar);
  window.fromArrow = "from arrow function";
};

testArrowFunc();
console.log("After arrow, window.fromArrow:", window.fromArrow);

console.log("\n=== Inside Nested Function ===");
function outer() {
  console.log("Outer function, globalThis.globalVar:", globalThis.globalVar);
  
  function inner() {
    console.log("Inner function, globalThis.globalVar:", globalThis.globalVar);
    console.log("Inner function, window.globalVar:", window.globalVar);
    window.fromNested = "from nested function";
  }
  
  inner();
}

outer();
console.log("After nested, window.fromNested:", window.fromNested);

console.log("\n=== Inside Constructor ===");
function MyClass() {
  console.log("Constructor, globalThis.globalVar:", globalThis.globalVar);
  console.log("Constructor, window.globalVar:", window.globalVar);
  console.log("Constructor, this === window:", this === window);
  console.log("Constructor, this === globalThis:", this === globalThis);
  window.fromConstructor = "from constructor";
}

new MyClass();
console.log("After constructor, window.fromConstructor:", window.fromConstructor);

console.log("\n=== Inside Method ===");
const obj = {
  method: function() {
    console.log("Method, globalThis.globalVar:", globalThis.globalVar);
    console.log("Method, window.globalVar:", window.globalVar);
    console.log("Method, this === window:", this === window);
    console.log("Method, this === globalThis:", this === globalThis);
    window.fromMethod = "from method";
  }
};

obj.method();
console.log("After method, window.fromMethod:", window.fromMethod);

console.log("\n=== All tests passed! ===");
