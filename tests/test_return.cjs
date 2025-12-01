// Test return in class methods

class Test {
  method1(x) {
    console.log("Method1: x =", x);
    if (x > 5) {
      console.log("Returning early");
      return;
    }
    console.log("After if block");
  }
  
  method2(x) {
    console.log("Method2: x =", x);
    if (x > 5) {
      console.log("Returning early with value");
      return "early";
    }
    console.log("After if block");
    return "normal";
  }
}

let t = new Test();

console.log("Test 1:");
t.method1(3);

console.log("\nTest 2:");
t.method1(7);

console.log("\nTest 3:");
let r1 = t.method2(3);
console.log("Returned:", r1);

console.log("\nTest 4:");
let r2 = t.method2(7);
console.log("Returned:", r2);
