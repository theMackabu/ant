// Test return in class methods

class Test {
  method1(x) {
    Ant.println("Method1: x =", x);
    if (x > 5) {
      Ant.println("Returning early");
      return;
    }
    Ant.println("After if block");
  }
  
  method2(x) {
    Ant.println("Method2: x =", x);
    if (x > 5) {
      Ant.println("Returning early with value");
      return "early";
    }
    Ant.println("After if block");
    return "normal";
  }
}

let t = new Test();

Ant.println("Test 1:");
t.method1(3);

Ant.println("\nTest 2:");
t.method1(7);

Ant.println("\nTest 3:");
let r1 = t.method2(3);
Ant.println("Returned:", r1);

Ant.println("\nTest 4:");
let r2 = t.method2(7);
Ant.println("Returned:", r2);
