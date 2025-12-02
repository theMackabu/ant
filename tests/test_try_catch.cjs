// Test 1: Basic try-catch
console.log("Test 1: Basic try-catch");
let result1 = "not caught";
try {
  throw new Error("test error");
  result1 = "should not reach";
} catch (e) {
  result1 = "caught: " + e.message;
}
console.log(result1);

// Test 2: Try without exception
console.log("\nTest 2: Try without exception");
let result2 = "initial";
try {
  result2 = "no error";
} catch (e) {
  result2 = "caught";
}
console.log(result2);

// Test 3: Try-catch-finally
console.log("\nTest 3: Try-catch-finally");
let result3 = [];
try {
  result3.push("try");
  throw new Error("error");
} catch (e) {
  result3.push("catch");
} finally {
  result3.push("finally");
}
console.log(result3.join(", "));

// Test 4: Finally without catch
console.log("\nTest 4: Try-finally (no exception)");
let result4 = [];
try {
  result4.push("try");
} finally {
  result4.push("finally");
}
console.log(result4.join(", "));

// Test 5: Catch without parentheses (optional binding)
console.log("\nTest 5: Catch without binding");
let result5 = "not caught";
try {
  throw "error string";
} catch {
  result5 = "caught without binding";
}
console.log(result5);

// Test 6: Nested try-catch
console.log("\nTest 6: Nested try-catch");
let result6 = [];
try {
  result6.push("outer try");
  try {
    result6.push("inner try");
    throw new Error("inner error");
  } catch (e) {
    result6.push("inner catch: " + e.message);
  }
  result6.push("after inner");
} catch (e) {
  result6.push("outer catch");
}
console.log(result6.join(", "));

// Test 7: Rethrow in catch
console.log("\nTest 7: Rethrow in catch");
let result7 = [];
try {
  try {
    throw new Error("original");
  } catch (e) {
    result7.push("first catch: " + e.message);
    throw new Error("rethrown");
  }
} catch (e) {
  result7.push("second catch: " + e.message);
}
console.log(result7.join(", "));

// Test 8: Finally runs even with return
console.log("\nTest 8: Finally with return");
function testFinallyReturn() {
  let x = [];
  try {
    x.push("try");
    return x;
  } finally {
    x.push("finally");
  }
}
console.log(testFinallyReturn().join(", "));

// Test 9: Access error properties
console.log("\nTest 9: Error properties");
try {
  throw new Error("test message");
} catch (e) {
  console.log("name: " + e.name);
  console.log("message: " + e.message);
}

// Test 10: Custom error class
console.log("\nTest 10: Custom error class");
class CustomError extends Error {
  constructor(msg) {
    super(msg);
    this.name = "CustomError";
  }
}

try {
  throw new CustomError("custom message");
} catch (e) {
  console.log("Caught " + e.name + ": " + e.message);
}

// Test 11: Finally modifies value
console.log("\nTest 11: Finally executes after catch");
let counter = 0;
try {
  counter = 1;
  throw new Error("test");
} catch (e) {
  counter = 2;
} finally {
  counter = counter + 10;
}
console.log("counter: " + counter);

// Test 12: Exception in finally propagates
console.log("\nTest 12: Exception in try, no catch");
let result12 = [];
try {
  try {
    throw new Error("original error");
  } finally {
    result12.push("finally ran");
  }
} catch (e) {
  result12.push("outer caught: " + e.message);
}
console.log(result12.join(", "));

console.log("\nAll tests completed!");
