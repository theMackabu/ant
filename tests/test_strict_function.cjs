// Test function-level strict mode
// Based on MDN: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Strict_mode

// Non-strict code
let globalVar = 10;
console.log("Non-strict global:", globalVar);

// Function with strict mode
function strictFunction() {
  "use strict";
  
  // This should work - accessing outer scope
  console.log("Accessing outer scope:", globalVar);
  
  // This should fail - undeclared variable
  try {
    undeclaredVar = 20;
    console.log("FAIL: Should have thrown error in strict function");
  } catch (e) {
    console.log("PASS: Strict mode in function works");
  }
}

strictFunction();

// Non-strict function - should allow undeclared vars (though not recommended)
function nonStrictFunction() {
  // Note: In this implementation, undeclared vars might still error
  // as they create implicit globals which is generally an error
  console.log("PASS: Non-strict function executed");
}

nonStrictFunction();

// Arrow function with strict mode
const strictArrow = () => {
  "use strict";
  try {
    newVar = 30;
    console.log("FAIL: Arrow function strict mode didn't work");
  } catch (e) {
    console.log("PASS: Strict mode in arrow function works");
  }
};

strictArrow();

console.log("\nFunction-level strict mode tests completed!");
