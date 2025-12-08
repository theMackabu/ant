// Test arrow function 'this' binding - arrows capture 'this' from surrounding scope

// Test 1: Arrow inside regular function captures outer this
let obj1 = {
  name: "test",
  regular() { return this.name; },
  method() {
    let inner = () => this.name;
    return inner();
  }
};
console.log("Test 1a - regular function this:", obj1.regular()); // Should be "test"
console.log("Test 1b - arrow captures outer this:", obj1.method()); // Should be "test"

// Test 2: Nested arrows maintain lexical this
let obj2 = {
  value: 42,
  outer() {
    let first = () => {
      let second = () => this.value;
      return second();
    };
    return first();
  }
};
console.log("Test 2 - nested arrows:", obj2.outer()); // Should be 42

// Test 3: Arrow in callback maintains this
let obj3 = {
  items: [1, 2, 3],
  multiplier: 10,
  process() {
    let results = [];
    let self = this;
    for (let i = 0; i < this.items.length; i++) {
      let fn = () => self.items[i] * self.multiplier;
      results[i] = fn();
    }
    return results;
  }
};
let result3 = obj3.process();
console.log("Test 3 - arrow in loop:", result3[0], result3[1], result3[2]); // Should be 10 20 30

// Test 4: Arrow function stored and called later still uses captured this
let obj4 = {
  secret: "hidden",
  getGetter() {
    return () => this.secret;
  }
};
let getter = obj4.getGetter();
console.log("Test 4 - stored arrow:", getter()); // Should be "hidden"

// Test 5: Arrow passed to different object still uses captured this
let obj5 = {
  id: "obj5",
  arrow() {
    return () => this.id;
  }
};
let arrowFromObj5 = obj5.arrow();
let obj6 = { id: "obj6", stolenArrow: arrowFromObj5 };
console.log("Test 5 - arrow keeps original this:", obj6.stolenArrow()); // Should still be "obj5"

console.log("\nAll arrow function this tests completed!");
